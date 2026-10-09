#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>

// Read-only decrypted partition view backed by nodtool's extracted files.
// Bytes outside extracted extents are deliberately rejected, not fabricated.
class DiscImage {
public:
    explicit DiscImage(const std::filesystem::path& root);
    std::vector<uint8_t> Read(uint64_t offset, uint32_t length) const;
private:
    struct Extent { uint64_t offset, size; std::filesystem::path path; };
    std::vector<Extent> extents;
    void Add(uint64_t offset, const std::filesystem::path& path, uint64_t expected);
};
