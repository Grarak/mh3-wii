#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

// The European RMHP08 disc image (RVZ, WIA or decrypted ISO), read in place.
// nod decodes RVZ/WIA chunks and the Wii encryption; a decrypted ISO is read
// sector by sector. Reads are positioned and serialized: any thread may read.
class RomImage {
  public:
    explicit RomImage(const std::filesystem::path& path);
    ~RomImage();
    RomImage(const RomImage&) = delete;

    // Size of the image file itself (identifies it in rom.txt).
    uint64_t FileBytes() const;

    // The game partition's decrypted data, 0x7c00 bytes per Wii sector.
    uint64_t PartitionBytes() const { return flatBytes; }
    void ReadPartition(uint64_t at, uint8_t* bytes, size_t count) const;
    std::vector<uint8_t> ReadPartition(uint64_t at, size_t count) const;

    // Raw disc bytes: headers, partition table, ticket, TMD and certificates.
    std::vector<uint8_t> ReadDisc(uint64_t at, size_t count) const;
    uint64_t PartitionOffset() const { return partition; }
    const std::vector<uint8_t>& PartitionHeader() const { return partitionHeader; }
#ifdef MH3_DEVELOPMENT
    // MH3_PROFILE=disc: logs the image's read speed, and the decoder's from
    // partition offset `sample` (disc_image.cpp).
    void Benchmark(uint64_t sample) const;
#endif

  private:
    class Source;
    std::unique_ptr<Source> source;
    mutable std::mutex lock;
    uint64_t partition = 0, dataStart = 0, flatBytes = 0;
    std::vector<uint8_t> partitionHeader;
};
