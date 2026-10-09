#include "disc_image.h"
#include "rom_image.h"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#ifdef __vita__
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// Keep the partition open and use positioned reads: no shared seek cursor,
// no reopen per DVD request, and native 64-bit offsets on Vita's 32-bit newlib.
class DiscFile {
  public:
    explicit DiscFile(const std::filesystem::path& path) : name(path.string()) {
#ifdef __vita__
        handle = sceIoOpen(name.c_str(), SCE_O_RDONLY, 0);
#else
        handle = ::open(name.c_str(), O_RDONLY);
#endif
        if (handle < 0)
            throw std::runtime_error("Cannot open disc file: " + name);
#ifdef __vita__
        SceIoStat stat{};
        const auto result = sceIoGetstatByFd(handle, &stat);
#else
        struct stat stat {};
        const auto result = ::fstat(handle, &stat);
#endif
        if (result < 0 || stat.st_size < 0) {
            Close();
            throw std::runtime_error("Cannot inspect disc file: " + name);
        }

        size = static_cast<uint64_t>(stat.st_size);
    }

    ~DiscFile() { Close(); }
    DiscFile(const DiscFile&) = delete;
    uint64_t Size() const { return size; }
    void Read(uint64_t offset, uint8_t* output, size_t length) const {
        if (offset > size || length > size - offset || offset > INT64_MAX)
            throw std::runtime_error("Disc read outside file: " + name);

        size_t done = 0;

        while (done < length) {
#ifdef __vita__
            const auto count = sceIoPread(handle, output + done, length - done, static_cast<SceOff>(offset + done));
#else
            const auto count = ::pread(handle, output + done, length - done, static_cast<off_t>(offset + done));

            if (count < 0 && errno == EINTR)
                continue;
#endif
            if (count <= 0)
                throw std::runtime_error("Disc file read failed: " + name);

            done += static_cast<size_t>(count);
        }
    }

  private:
    void Close() {
#ifdef __vita__
        sceIoClose(handle);
#else
        ::close(handle);
#endif
    }

    std::string name;
    int handle = -1;
    uint64_t size = 0;
};

namespace {
bool Exists(const std::filesystem::path& path) {
#ifdef __vita__
    SceIoStat stat{};
    const auto result = sceIoGetstat(path.string().c_str(), &stat);

    if (result >= 0)
        return true;

    if (static_cast<uint32_t>(result) == 0x80010002)
        return false;

    throw std::runtime_error("Cannot inspect disc path: " + path.string());
#else
    return std::filesystem::exists(path);
#endif
}

std::vector<uint8_t> Load(const std::filesystem::path& path) {
    DiscFile file(path);

    if (file.Size() > 64 * 1024 * 1024)
        throw std::runtime_error("Invalid disc metadata size");

    std::vector<uint8_t> bytes(static_cast<size_t>(file.Size()));
    file.Read(0, bytes.data(), bytes.size());
    return bytes;
}

uint32_t Word(const std::vector<uint8_t>& data, size_t offset) {
    if (offset > data.size() || data.size() - offset < 4)
        throw std::runtime_error("Truncated disc metadata");

    return uint32_t(data[offset]) << 24 | uint32_t(data[offset + 1]) << 16 | uint32_t(data[offset + 2]) << 8 | data[offset + 3];
}
} // namespace

void DiscImage::Add(uint64_t offset, const std::filesystem::path& path, uint64_t expected, bool verify) {
    if (verify && DiscFile(path).Size() != expected)
        throw std::runtime_error("Extracted file length differs from disc metadata: " + path.string());

    if (expected)
        extents.push_back({offset, expected, path});
}

DiscImage::~DiscImage() = default;
DiscImage::DiscImage(const std::filesystem::path& root, bool applyNativeTextures) {
    const auto boot = Load(root / "sys/boot.bin");
    const auto fst = Load(root / "sys/fst.bin");

    if (Exists(root / "disc-layout.txt")) {
        const auto layout = Load(root / "disc-layout.txt");

        if (std::string(layout.begin(), layout.end()) != "mh3-partition-v1\n")
            throw std::runtime_error("Unsupported disc layout");

        packed = true;
    }

    if (boot.size() != 0x440 || Word(boot, 0x18) != 0x5D1C9EA3)
        throw std::runtime_error("Expected an extracted Wii partition");

    Add(0, root / "sys/boot.bin", 0x440);
    Add(0x440, root / "sys/bi2.bin", 0x2000);
    Add(0x2440, root / "sys/apploader.img", DiscFile(root / "sys/apploader.img").Size());
    Add(uint64_t(Word(boot, 0x420)) * 4, root / "sys/main.dol", DiscFile(root / "sys/main.dol").Size());
    Add(uint64_t(Word(boot, 0x424)) * 4, root / "sys/fst.bin", uint64_t(Word(boot, 0x428)) * 4);
    const uint32_t count = Word(fst, 8);

    if (!count || uint64_t(count) * 12 > fst.size() || (Word(fst, 0) >> 24) != 1)
        throw std::runtime_error("Invalid FST root");

    struct Directory {
        uint32_t index, end;
        std::filesystem::path path;
    };

    std::vector<Directory> stack{{0, count, root / "files"}};

    for (uint32_t index = 1; index < count; ++index) {
        while (index >= stack.back().end)
            stack.pop_back();

        const uint32_t nameWord = Word(fst, index * 12);
        const size_t nameOffset = size_t(count) * 12 + (nameWord & 0xFFFFFF);

        if (nameOffset >= fst.size())
            throw std::runtime_error("Invalid FST name offset");

        auto end = std::find(fst.begin() + nameOffset, fst.end(), 0);

        if (end == fst.end())
            throw std::runtime_error("Unterminated FST name");

        std::string name(fst.begin() + nameOffset, end);

        if (name.empty() || name == "." || name == ".." || name.find('/') != name.npos || name.find('\\') != name.npos || name.find(':') != name.npos)
            throw std::runtime_error("Invalid FST path component");

        auto path = stack.back().path / name;
        const uint32_t offset = Word(fst, index * 12 + 4), size = Word(fst, index * 12 + 8);

        if ((nameWord >> 24) == 1) {
            if (offset != stack.back().index || size <= index || size > stack.back().end)
                throw std::runtime_error("Invalid FST directory bounds");

            stack.push_back({index, size, path});
        } else if ((nameWord >> 24) == 0) {
            const auto relative = path.lexically_relative(root / "files").generic_string();

            if (!fileEntries.emplace(relative, std::pair{uint64_t(offset) * 4, size}).second)
                throw std::runtime_error("Duplicate FST file name");

            Add(uint64_t(offset) * 4, path, size, !packed);
        } else
            throw std::runtime_error("Invalid FST entry type");
    }

    std::sort(extents.begin(), extents.end(), [](const auto& a, const auto& b) { return a.offset < b.offset; });

    for (size_t i = 1; i < extents.size(); ++i)
        if (extents[i - 1].offset + extents[i - 1].size > extents[i].offset)
            throw std::runtime_error("Overlapping extracted disc extents");
    const auto partitionPath = root / "partition.bin";

    if (Exists(partitionPath)) {
        partition = std::make_unique<DiscFile>(partitionPath);
        partitionSize = partition->Size();

        if (partitionSize < extents.back().offset + extents.back().size)
            throw std::runtime_error("Truncated preserved partition");

        const auto verify = [&](uint64_t offset, const std::vector<uint8_t>& expected) {
            std::vector<uint8_t> actual(expected.size());
            partition->Read(offset, actual.data(), actual.size());

            if (actual != expected)
                throw std::runtime_error("Preserved partition differs from extracted disc metadata");
        };

        verify(0, boot);
        verify(uint64_t(Word(boot, 0x424)) * 4, fst);
    } else if (packed && Exists(root / "rom.txt")) {
        // The game partition is read in place from the disc image.
        std::ifstream list((root / "rom.txt").string());
        std::string path;
        uint64_t bytes = 0;

        if (!std::getline(list, path) || !(list >> bytes))
            throw std::runtime_error("Invalid rom.txt in " + root.string());

        rom = std::make_unique<RomImage>(path);

        if (rom->FileBytes() != bytes)
            throw std::runtime_error("The disc image changed since its game data was prepared: " + path);

        partitionSize = rom->PartitionBytes();

        if (partitionSize < extents.back().offset + extents.back().size)
            throw std::runtime_error("Disc image partition is shorter than its file table");

        std::vector<uint8_t> actual(fst.size());
        rom->ReadPartition(uint64_t(Word(boot, 0x424)) * 4, actual.data(), actual.size());

        if (actual != fst)
            throw std::runtime_error("Disc image differs from the prepared disc metadata");
    } else if (packed)
        throw std::runtime_error("Packed disc requires partition.bin or rom.txt");
    // A development deployment can select a complete versioned native asset
    // set without replacing the preserved disc or another processed generation.
    const char* nativeRoot = std::getenv("MH3_NATIVE_ASSET_DIR");
    const auto assetRoot = nativeRoot ? std::filesystem::path(nativeRoot) : root;
    const auto mapPath = assetRoot / "native-texture-map.bin";

    if (applyNativeTextures && Exists(mapPath)) {
        hasTextureMap = true;

        if (!Exists(assetRoot / "native-textures.bin"))
            throw std::runtime_error("Native texture map requires its payload pack");

        const auto map = Load(mapPath);
        const auto little32 = [&](size_t at) {
            return uint32_t(map[at]) | uint32_t(map[at + 1]) << 8 | uint32_t(map[at + 2]) << 16 | uint32_t(map[at + 3]) << 24;
        };

        if (map.size() < 12 || little32(0) != 0x4d54334d || little32(4) != 1 || little32(8) > 32768 || map.size() != 12 + size_t(little32(8)) * 12)
            throw std::runtime_error("Invalid native texture map");

        uint64_t previousEnd = 0;

        for (size_t at = 12; at < map.size(); at += 12) {
            const uint64_t offset = uint64_t(little32(at)) | uint64_t(little32(at + 4)) << 32;

            if (offset < previousEnd || offset > UINT64_MAX - 4 || (partitionSize && offset + 4 > partitionSize) || (map[at + 8] & 0xf0) != 0xa0)
                throw std::runtime_error("Invalid native texture tag offset");

            nativeTexturePatches.push_back({offset, {map[at + 8], map[at + 9], map[at + 10], map[at + 11]}});
            previousEnd = offset + 4;
        }
    }
}

void DiscImage::ReadPartition(uint64_t offset, uint8_t* output, size_t length) const {
    if (partition)
        partition->Read(offset, output, length);
    else if (rom)
        rom->ReadPartition(offset, output, length);
    else
        throw std::runtime_error("Disc has no partition data");
}

void DiscImage::ApplyNativeTexturePatches(uint64_t offset, std::vector<uint8_t>& data) const {
    const uint64_t end = offset + data.size();
    auto it = std::lower_bound(nativeTexturePatches.begin(), nativeTexturePatches.end(), offset, [](const NativeTexturePatch& patch, uint64_t start) { return patch.offset + 4 <= start; });

    for (; it != nativeTexturePatches.end() && it->offset < end; ++it)
        for (unsigned byte = 0; byte < 4; ++byte)
            if (it->offset + byte >= offset && it->offset + byte < end)
                data[it->offset + byte - offset] = it->bytes[byte];
}

void DiscImage::ReadRaw(uint64_t offset, uint8_t* output, size_t length) const {
    if (packed) {
        ReadPartition(offset, output, length);
        return;
    }

    size_t done = 0;

    while (done < length) {
        auto it = std::upper_bound(extents.begin(), extents.end(), offset, [](uint64_t value, const Extent& extent) { return value < extent.offset; });

        if (it == extents.begin())
            throw std::runtime_error("Disc read precedes extracted data");

        --it;

        if (offset >= it->offset + it->size) {
            if (!partitionSize || offset >= partitionSize)
                throw std::runtime_error("Disc read reaches unextracted padding at byte " + std::to_string(offset));

            auto next = it + 1;
            const uint64_t end = next == extents.end() ? partitionSize : std::min(partitionSize, next->offset);
            const size_t count = std::min<uint64_t>(length - done, end - offset);
            ReadPartition(offset, output + done, count);
            offset += count;
            done += count;
            continue;
        }

        const size_t count = std::min<uint64_t>(length - done, it->offset + it->size - offset);
        DiscFile(it->path).Read(offset - it->offset, output + done, count);
        offset += count;
        done += count;
    }
}

std::vector<uint8_t> DiscImage::Read(uint64_t offset, uint32_t length) const {
    if (length > 64 * 1024 * 1024 || offset > std::numeric_limits<uint64_t>::max() - length)
        throw std::runtime_error("Disc read exceeds supported request size");

    std::vector<uint8_t> result(length);

    if (reader)
        reader(offset, result.data(), length);
    else {
        ReadRaw(offset, result.data(), length);
        ApplyNativeTexturePatches(offset, result);
    }

    ApplyFileOverrides(offset, result);
    return result;
}

std::vector<uint8_t> DiscImage::ReadFile(const std::string& name) const {
    const auto it = fileEntries.find(name);

    if (it == fileEntries.end())
        throw std::runtime_error("Missing FST file: " + name);

    return Read(it->second.first, it->second.second);
}

void DiscImage::SetFileOverride(const std::string& name, std::vector<uint8_t> bytes) {
    const auto it = fileEntries.find(name);

    if (it == fileEntries.end() || bytes.size() != it->second.second)
        throw std::runtime_error("Quest override must preserve the disc file length");

    if (name != "05/quest00_eng.bin" && name != "05/quest01_eng.bin" && name != "05/quest03_eng.bin")
        throw std::runtime_error("Only quest files may be overridden");

    fileOverrides[it->second.first] = std::move(bytes);
}

void DiscImage::ApplyFileOverrides(uint64_t offset, std::vector<uint8_t>& data) const {
    for (const auto& [start, bytes] : fileOverrides) {
        const auto first = std::max(offset, start), last = std::min(offset + data.size(), start + bytes.size());

        if (first < last)
            std::copy(bytes.begin() + (first - start), bytes.begin() + (last - start), data.begin() + (first - offset));
    }
}

std::vector<std::string> DiscImage::FileNames() const {
    std::vector<std::string> names;

    for (const auto& [name, extent] : fileEntries)
        names.push_back(name);

    return names;
}

uint64_t DiscImage::FileOffset(const std::string& name) const {
    const auto it = fileEntries.find(name);

    if (it == fileEntries.end())
        throw std::runtime_error("Missing FST file: " + name);

    return it->second.first;
}

uint32_t DiscImage::FileSize(const std::string& name) const {
    const auto it = fileEntries.find(name);

    if (it == fileEntries.end())
        throw std::runtime_error("Missing FST file: " + name);

    return it->second.second;
}
