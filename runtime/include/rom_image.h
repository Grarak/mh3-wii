#pragma once
#include "status.h"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

// The European RMHP08 disc image (RVZ, WIA or decrypted ISO), read in place.
// The native decoder (rom_decoder.cpp) decodes RVZ/WIA chunks and the Wii
// encryption; a decrypted ISO is read sector by sector. Reads are positioned and serialized: any thread may read.
class RomImage {
  public:
    // Opens the image and checks that it is the European game disc.
    static Status Open(const std::filesystem::path& path, std::unique_ptr<RomImage>& image);
    ~RomImage();
    RomImage(const RomImage&) = delete;

    // Size of the image file itself (identifies it in rom.txt).
    uint64_t FileBytes() const;

    // The game partition's decrypted data, 0x7c00 bytes per Wii sector.
    uint64_t PartitionBytes() const { return flatBytes; }
    Status ReadPartition(uint64_t at, uint8_t* bytes, size_t count) const;
    Status ReadPartition(uint64_t at, size_t count, std::vector<uint8_t>& bytes) const;

    // Raw disc bytes: headers, partition table, ticket, TMD and certificates.
    Status ReadDisc(uint64_t at, size_t count, std::vector<uint8_t>& bytes) const;
    uint64_t PartitionOffset() const { return partition; }
    const std::vector<uint8_t>& PartitionHeader() const { return partitionHeader; }
#ifdef MH3_DEVELOPMENT
    // MH3_PROFILE=disc: logs the image's read speed, and the decoder's from
    // partition offset `sample` (disc_image.cpp).
    void Benchmark(uint64_t sample) const;
#endif

  private:
    RomImage();
    Status Load(const std::filesystem::path& path);
    class Source;
    std::unique_ptr<Source> source;
    mutable std::mutex lock;
    uint64_t partition = 0, dataStart = 0, flatBytes = 0;
    std::vector<uint8_t> partitionHeader;
};
