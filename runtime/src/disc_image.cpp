#include "disc_image.h"
#include <algorithm>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
std::vector<uint8_t> Load(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Missing extracted disc file: " + path.string());
    const auto size = file.tellg();
    if (size < 0 || size > 64 * 1024 * 1024) throw std::runtime_error("Invalid disc metadata size");
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    if (!file) throw std::runtime_error("Unable to read disc metadata");
    return bytes;
}
uint32_t Word(const std::vector<uint8_t>& data, size_t offset) {
    if (offset > data.size() || data.size() - offset < 4) throw std::runtime_error("Truncated disc metadata");
    return uint32_t(data[offset]) << 24 | uint32_t(data[offset+1]) << 16 |
           uint32_t(data[offset+2]) << 8 | data[offset+3];
}
}
void DiscImage::Add(uint64_t offset, const std::filesystem::path& path, uint64_t expected) {
    if (std::filesystem::file_size(path) != expected)
        throw std::runtime_error("Extracted file length differs from disc metadata: " + path.string());
    if (expected) extents.push_back({offset, expected, path});
}
DiscImage::DiscImage(const std::filesystem::path& root) {
    const auto boot = Load(root / "sys/boot.bin");
    const auto fst = Load(root / "sys/fst.bin");
    if (boot.size() != 0x440 || Word(boot, 0x18) != 0x5D1C9EA3)
        throw std::runtime_error("Expected an extracted Wii partition");
    Add(0, root / "sys/boot.bin", 0x440);
    Add(0x440, root / "sys/bi2.bin", 0x2000);
    Add(0x2440, root / "sys/apploader.img", std::filesystem::file_size(root / "sys/apploader.img"));
    Add(uint64_t(Word(boot, 0x420)) * 4, root / "sys/main.dol", std::filesystem::file_size(root / "sys/main.dol"));
    Add(uint64_t(Word(boot, 0x424)) * 4, root / "sys/fst.bin", uint64_t(Word(boot, 0x428)) * 4);
    const uint32_t count = Word(fst, 8);
    if (!count || uint64_t(count) * 12 > fst.size() || (Word(fst, 0) >> 24) != 1)
        throw std::runtime_error("Invalid FST root");
    struct Directory { uint32_t index, end; std::filesystem::path path; };
    std::vector<Directory> stack{{0, count, root / "files"}};
    for (uint32_t index = 1; index < count; ++index) {
        while (index >= stack.back().end) stack.pop_back();
        const uint32_t nameWord = Word(fst, index * 12);
        const size_t nameOffset = size_t(count) * 12 + (nameWord & 0xFFFFFF);
        if (nameOffset >= fst.size()) throw std::runtime_error("Invalid FST name offset");
        auto end = std::find(fst.begin() + nameOffset, fst.end(), 0);
        if (end == fst.end()) throw std::runtime_error("Unterminated FST name");
        std::string name(fst.begin() + nameOffset, end);
        if (name.empty() || name == "." || name == ".." || name.find('/') != name.npos || name.find('\\') != name.npos)
            throw std::runtime_error("Invalid FST path component");
        auto path = stack.back().path / name;
        const uint32_t offset = Word(fst, index * 12 + 4), size = Word(fst, index * 12 + 8);
        if ((nameWord >> 24) == 1) {
            if (offset != stack.back().index || size <= index || size > stack.back().end)
                throw std::runtime_error("Invalid FST directory bounds");
            stack.push_back({index, size, path});
        } else if ((nameWord >> 24) == 0) Add(uint64_t(offset) * 4, path, size);
        else throw std::runtime_error("Invalid FST entry type");
    }
    std::sort(extents.begin(), extents.end(), [](const auto& a, const auto& b) { return a.offset < b.offset; });
    for (size_t i = 1; i < extents.size(); ++i)
        if (extents[i-1].offset + extents[i-1].size > extents[i].offset)
            throw std::runtime_error("Overlapping extracted disc extents");
}
std::vector<uint8_t> DiscImage::Read(uint64_t offset, uint32_t length) const {
    if (length > 64 * 1024 * 1024 || offset > std::numeric_limits<uint64_t>::max() - length)
        throw std::runtime_error("Disc read exceeds supported request size");
    std::vector<uint8_t> result(length);
    size_t done = 0;
    while (done < length) {
        auto it = std::upper_bound(extents.begin(), extents.end(), offset,
            [](uint64_t value, const Extent& extent) { return value < extent.offset; });
        if (it == extents.begin()) throw std::runtime_error("Disc read precedes extracted data");
        --it;
        if (offset >= it->offset + it->size)
            throw std::runtime_error("Disc read reaches unextracted padding at byte " + std::to_string(offset));
        const size_t count = std::min<uint64_t>(length - done, it->offset + it->size - offset);
        std::ifstream file(it->path, std::ios::binary);
        file.seekg(offset - it->offset);
        file.read(reinterpret_cast<char*>(result.data() + done), count);
        if (!file) throw std::runtime_error("Extracted disc read failed: " + it->path.string());
        offset += count; done += count;
    }
    return result;
}
