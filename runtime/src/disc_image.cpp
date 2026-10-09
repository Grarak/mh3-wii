#include "disc_image.h"
#include "fatal.h"
#include "dev_options.h"
#include "rom_image.h"
#include "fs_status.h"
#include "status.h"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <atomic>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#ifdef __vita__
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include "../vita/aligned_io.h"
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// Keep the partition open and use positioned reads: no shared seek cursor,
// no reopen per DVD request, and native 64-bit offsets on Vita's 32-bit newlib.
// On the Vita a suspend (the PS button) invalidates the descriptors of files
// open under ux0:data: after the resume every read fails with ENODEV
// (0x80010013, Sony's Development Process Overview), so the reader reopens
// the file once and retries.
class DiscFile {
  public:
    DiscFile() = default;

    Status Open(const std::filesystem::path& path) {
        name = path.string();
#ifdef __vita__
        handle = sceIoOpen(name.c_str(), SCE_O_RDONLY, 0);
#else
        handle = ::open(name.c_str(), O_RDONLY);
#endif
        if (handle < 0)
            return Status::Error("Cannot open disc file: " + name);
#ifdef __vita__
        SceIoStat stat{};
        const auto result = sceIoGetstatByFd(handle, &stat);
#else
        struct stat stat {};
        const auto result = ::fstat(handle, &stat);
#endif
        if (result < 0 || stat.st_size < 0) {
            Close();
            return Status::Error("Cannot inspect disc file: " + name);
        }

        size = static_cast<uint64_t>(stat.st_size);
        return {};
    }

    ~DiscFile() { Close(); }
    DiscFile(const DiscFile&) = delete;
    uint64_t Size() const { return size; }
    Status Read(uint64_t offset, uint8_t* output, size_t length) const {
        if (offset > size || length > size - offset || offset > INT64_MAX)
            return Status::Error("Disc read outside file: " + name);

        size_t done = 0;
#ifdef __vita__
        bool reopened = false;
#endif

        while (done < length) {
#ifdef __vita__
            const int file = handle.load(std::memory_order_acquire);
            const auto count = VitaIo::Pread(file, output + done, length - done, offset + done);

            // Invalidated by a suspend (or closed by another reader's reopen).
            if (count < 0 && !reopened) {
                reopened = true;

                if (Reopen(file))
                    continue;
            }
#else
            const auto count = ::pread(handle, output + done, length - done, static_cast<off_t>(offset + done));

            if (count < 0 && errno == EINTR)
                continue;
#endif
            if (count <= 0)
                return Status::Error("Disc file read failed: " + name);

            done += static_cast<size_t>(count);
        }

        return {};
    }

  private:
#ifdef __vita__
    // Replaces `stale` with a new descriptor, unless another reader already
    // did; false when the file cannot be opened again.
    bool Reopen(int stale) const {
        std::lock_guard lock(reopenLock);

        if (handle.load(std::memory_order_relaxed) != stale)
            return true;

        const int file = sceIoOpen(name.c_str(), SCE_O_RDONLY, 0);

        if (file < 0)
            return false;

        sceIoClose(stale);
        handle.store(file, std::memory_order_release);
        return true;
    }
#endif

    void Close() {
        const int file = handle.exchange(-1);

        if (file < 0)
            return;
#ifdef __vita__
        sceIoClose(file);
#else
        ::close(file);
#endif
    }

    std::string name;
    mutable std::atomic<int> handle{-1};
#ifdef __vita__
    mutable std::mutex reopenLock;
#endif
    uint64_t size = 0;
};

namespace {
Status Exists(const std::filesystem::path& path, bool& exists) {
#ifdef __vita__
    SceIoStat stat{};
    const auto result = sceIoGetstat(path.string().c_str(), &stat);
    exists = result >= 0;

    if (result >= 0 || static_cast<uint32_t>(result) == 0x80010002)
        return {};

    return Status::Error("Cannot inspect disc path: " + path.string());
#else
    return FsStatus::Exists(path, exists);
#endif
}

Status LoadFile(const std::filesystem::path& path, std::vector<uint8_t>& bytes) {
    DiscFile file;
    MH3_RETURN_IF_ERROR(file.Open(path));

    if (file.Size() > 64 * 1024 * 1024)
        return Status::Error("Invalid disc metadata size");

    bytes.resize(static_cast<size_t>(file.Size()));
    return file.Read(0, bytes.data(), bytes.size());
}

Status FileBytes(const std::filesystem::path& path, uint64_t& size) {
    DiscFile file;
    MH3_RETURN_IF_ERROR(file.Open(path));
    size = file.Size();
    return {};
}

uint32_t Word(const std::vector<uint8_t>& data, size_t offset) {
    if (offset > data.size() || data.size() - offset < 4)
        MH3_THROW(std::runtime_error("Truncated disc metadata"));

    return uint32_t(data[offset]) << 24 | uint32_t(data[offset + 1]) << 16 | uint32_t(data[offset + 2]) << 8 | data[offset + 3];
}
} // namespace

Status DiscImage::Add(uint64_t offset, const std::filesystem::path& path, uint64_t expected, bool verify) {
    if (verify) {
        uint64_t size = 0;
        MH3_RETURN_IF_ERROR(FileBytes(path, size));

        if (size != expected)
            return Status::Error("Extracted file length differs from disc metadata: " + path.string());
    }

    if (expected)
        extents.push_back({offset, expected, path});

    return {};
}

DiscImage::DiscImage() = default;
DiscImage::~DiscImage() = default;

DiscImage::DiscImage(const std::filesystem::path& root, bool applyNativeTextures) {
    CheckStatus(Load(root, applyNativeTextures));
}

// Word reads metadata only where its length has been checked.
Status DiscImage::Load(const std::filesystem::path& root, bool applyNativeTextures) {
    std::vector<uint8_t> boot, fst;
    MH3_RETURN_IF_ERROR(LoadFile(root / "sys/boot.bin", boot));
    MH3_RETURN_IF_ERROR(LoadFile(root / "sys/fst.bin", fst));
    bool exists = false;
    MH3_RETURN_IF_ERROR(Exists(root / "disc-layout.txt", exists));

    if (exists) {
        std::vector<uint8_t> layout;
        MH3_RETURN_IF_ERROR(LoadFile(root / "disc-layout.txt", layout));

        if (std::string(layout.begin(), layout.end()) != "mh3-partition-v1\n")
            return Status::Error("Unsupported disc layout");

        packed = true;
    }

    if (boot.size() != 0x440 || Word(boot, 0x18) != 0x5D1C9EA3)
        return Status::Error("Expected an extracted Wii partition");

    uint64_t apploader = 0, dol = 0;
    MH3_RETURN_IF_ERROR(Add(0, root / "sys/boot.bin", 0x440));
    MH3_RETURN_IF_ERROR(Add(0x440, root / "sys/bi2.bin", 0x2000));
    MH3_RETURN_IF_ERROR(FileBytes(root / "sys/apploader.img", apploader));
    MH3_RETURN_IF_ERROR(Add(0x2440, root / "sys/apploader.img", apploader));
    MH3_RETURN_IF_ERROR(FileBytes(root / "sys/main.dol", dol));
    MH3_RETURN_IF_ERROR(Add(uint64_t(Word(boot, 0x420)) * 4, root / "sys/main.dol", dol));
    MH3_RETURN_IF_ERROR(Add(uint64_t(Word(boot, 0x424)) * 4, root / "sys/fst.bin", uint64_t(Word(boot, 0x428)) * 4));

    if (fst.size() < 12)
        return Status::Error("Truncated disc metadata");

    const uint32_t count = Word(fst, 8);

    if (!count || uint64_t(count) * 12 > fst.size() || (Word(fst, 0) >> 24) != 1)
        return Status::Error("Invalid FST root");

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
            return Status::Error("Invalid FST name offset");

        auto end = std::find(fst.begin() + nameOffset, fst.end(), 0);

        if (end == fst.end())
            return Status::Error("Unterminated FST name");

        std::string name(fst.begin() + nameOffset, end);

        if (name.empty() || name == "." || name == ".." || name.find('/') != name.npos || name.find('\\') != name.npos || name.find(':') != name.npos)
            return Status::Error("Invalid FST path component");

        auto path = stack.back().path / name;
        const uint32_t offset = Word(fst, index * 12 + 4), size = Word(fst, index * 12 + 8);

        if ((nameWord >> 24) == 1) {
            if (offset != stack.back().index || size <= index || size > stack.back().end)
                return Status::Error("Invalid FST directory bounds");

            stack.push_back({index, size, path});
        } else if ((nameWord >> 24) == 0) {
            const auto relative = path.lexically_relative(root / "files").generic_string();

            if (!fileEntries.emplace(relative, std::pair{uint64_t(offset) * 4, size}).second)
                return Status::Error("Duplicate FST file name");

            MH3_RETURN_IF_ERROR(Add(uint64_t(offset) * 4, path, size, !packed));
        } else
            return Status::Error("Invalid FST entry type");
    }

    std::sort(extents.begin(), extents.end(), [](const auto& a, const auto& b) { return a.offset < b.offset; });

    for (size_t i = 1; i < extents.size(); ++i)
        if (extents[i - 1].offset + extents[i - 1].size > extents[i].offset)
            return Status::Error("Overlapping extracted disc extents");
    const auto partitionPath = root / "partition.bin";
    bool preserved = false, recorded = false;
    MH3_RETURN_IF_ERROR(Exists(partitionPath, preserved));

    if (!preserved && packed)
        MH3_RETURN_IF_ERROR(Exists(root / "rom.txt", recorded));

    if (preserved) {
        partition = std::make_unique<DiscFile>();
        MH3_RETURN_IF_ERROR(partition->Open(partitionPath));
        partitionSize = partition->Size();

        if (partitionSize < extents.back().offset + extents.back().size)
            return Status::Error("Truncated preserved partition");

        const auto verify = [&](uint64_t offset, const std::vector<uint8_t>& expected) {
            std::vector<uint8_t> actual(expected.size());
            MH3_RETURN_IF_ERROR(partition->Read(offset, actual.data(), actual.size()));

            if (actual != expected)
                return Status::Error("Preserved partition differs from extracted disc metadata");

            return Status{};
        };

        MH3_RETURN_IF_ERROR(verify(0, boot));
        MH3_RETURN_IF_ERROR(verify(uint64_t(Word(boot, 0x424)) * 4, fst));
    } else if (packed && recorded) {
        // The game partition is read in place from the disc image.
        std::ifstream list((root / "rom.txt").string());
        std::string path;
        uint64_t bytes = 0;

        if (!std::getline(list, path) || !(list >> bytes))
            return Status::Error("Invalid rom.txt in " + root.string());

        MH3_RETURN_IF_ERROR(RomImage::Open(path, rom));

        if (rom->FileBytes() != bytes)
            return Status::Error("The disc image changed since its game data was prepared: " + path);

#ifdef MH3_DEVELOPMENT
        if (ProfileOptions().Has("disc")) {
            // Decode game data: the largest model archive.
            std::pair<uint64_t, uint32_t> sample{0, 0};

            for (const auto& [name, entry] : fileEntries)
                if (name.ends_with(".brres") && entry.second > sample.second)
                    sample = entry;

            rom->Benchmark(sample.first);
        }
#endif

        partitionSize = rom->PartitionBytes();

        if (partitionSize < extents.back().offset + extents.back().size)
            return Status::Error("Disc image partition is shorter than its file table");

        std::vector<uint8_t> actual(fst.size());
        MH3_RETURN_IF_ERROR(rom->ReadPartition(uint64_t(Word(boot, 0x424)) * 4, actual.data(), actual.size()));

        if (actual != fst)
            return Status::Error("Disc image differs from the prepared disc metadata");
    } else if (packed)
        return Status::Error("Packed disc requires partition.bin or rom.txt");
    // A development deployment can select a complete versioned native asset
    // set without replacing the preserved disc or another processed generation.
    const char* nativeRoot = std::getenv("MH3_NATIVE_ASSET_DIR");
    const auto assetRoot = nativeRoot ? std::filesystem::path(nativeRoot) : root;
    const auto mapPath = assetRoot / "native-texture-map.bin";
    bool mapped = false;

    if (applyNativeTextures)
        MH3_RETURN_IF_ERROR(Exists(mapPath, mapped));

    if (mapped) {
        hasTextureMap = true;
        bool payload = false;
        MH3_RETURN_IF_ERROR(Exists(assetRoot / "native-textures.bin", payload));

        if (!payload)
            return Status::Error("Native texture map requires its payload pack");

        std::vector<uint8_t> map;
        MH3_RETURN_IF_ERROR(LoadFile(mapPath, map));
        const auto little32 = [&](size_t at) {
            return uint32_t(map[at]) | uint32_t(map[at + 1]) << 8 | uint32_t(map[at + 2]) << 16 | uint32_t(map[at + 3]) << 24;
        };

        if (map.size() < 12 || little32(0) != 0x4d54334d || little32(4) != 1 || little32(8) > 32768 || map.size() != 12 + size_t(little32(8)) * 12)
            return Status::Error("Invalid native texture map");

        uint64_t previousEnd = 0;

        for (size_t at = 12; at < map.size(); at += 12) {
            const uint64_t offset = uint64_t(little32(at)) | uint64_t(little32(at + 4)) << 32;

            if (offset < previousEnd || offset > UINT64_MAX - 4 || (partitionSize && offset + 4 > partitionSize) || (map[at + 8] & 0xf0) != 0xa0)
                return Status::Error("Invalid native texture tag offset");

            nativeTexturePatches.push_back({offset, {map[at + 8], map[at + 9], map[at + 10], map[at + 11]}});
            previousEnd = offset + 4;
        }
    }

    return {};
}

Status DiscImage::ReadPartition(uint64_t offset, uint8_t* output, size_t length) const {
    if (partition)
        return partition->Read(offset, output, length);

    if (rom)
        return rom->ReadPartition(offset, output, length);

    return Status::Error("Disc has no partition data");
}

void DiscImage::ApplyNativeTexturePatches(uint64_t offset, std::vector<uint8_t>& data) const {
    const uint64_t end = offset + data.size();
    auto it = std::lower_bound(nativeTexturePatches.begin(), nativeTexturePatches.end(), offset, [](const NativeTexturePatch& patch, uint64_t start) { return patch.offset + 4 <= start; });

    for (; it != nativeTexturePatches.end() && it->offset < end; ++it)
        for (unsigned byte = 0; byte < 4; ++byte)
            if (it->offset + byte >= offset && it->offset + byte < end)
                data[it->offset + byte - offset] = it->bytes[byte];
}

Status DiscImage::ReadRaw(uint64_t offset, uint8_t* output, size_t length) const {
    if (packed)
        return ReadPartition(offset, output, length);

    size_t done = 0;

    while (done < length) {
        auto it = std::upper_bound(extents.begin(), extents.end(), offset, [](uint64_t value, const Extent& extent) { return value < extent.offset; });

        if (it == extents.begin())
            return Status::Error("Disc read precedes extracted data");

        --it;

        if (offset >= it->offset + it->size) {
            if (!partitionSize || offset >= partitionSize)
                return Status::Error("Disc read reaches unextracted padding at byte " + std::to_string(offset));

            auto next = it + 1;
            const uint64_t end = next == extents.end() ? partitionSize : std::min(partitionSize, next->offset);
            const size_t count = std::min<uint64_t>(length - done, end - offset);
            MH3_RETURN_IF_ERROR(ReadPartition(offset, output + done, count));
            offset += count;
            done += count;
            continue;
        }

        const size_t count = std::min<uint64_t>(length - done, it->offset + it->size - offset);
        DiscFile file;
        MH3_RETURN_IF_ERROR(file.Open(it->path));
        MH3_RETURN_IF_ERROR(file.Read(offset - it->offset, output + done, count));
        offset += count;
        done += count;
    }

    return {};
}

Status DiscImage::TryRead(uint64_t offset, uint32_t length, std::vector<uint8_t>& result) const {
    if (length > 64 * 1024 * 1024 || offset > std::numeric_limits<uint64_t>::max() - length)
        return Status::Error("Disc read exceeds supported request size");

    result.resize(length);

    if (reader)
        MH3_RETURN_IF_ERROR(reader(offset, result.data(), length));
    else {
        MH3_RETURN_IF_ERROR(ReadRaw(offset, result.data(), length));
        ApplyNativeTexturePatches(offset, result);
    }

    ApplyFileOverrides(offset, result);
    return {};
}

std::vector<uint8_t> DiscImage::Read(uint64_t offset, uint32_t length) const {
    std::vector<uint8_t> result;
    CheckStatus(TryRead(offset, length, result));
    return result;
}

std::vector<uint8_t> DiscImage::ReadFile(const std::string& name) const {
    const auto it = fileEntries.find(name);

    if (it == fileEntries.end())
        MH3_THROW(std::runtime_error("Missing FST file: " + name));

    return Read(it->second.first, it->second.second);
}

void DiscImage::SetFileOverride(const std::string& name, std::vector<uint8_t> bytes) {
    const auto it = fileEntries.find(name);

    if (it == fileEntries.end() || bytes.size() != it->second.second)
        MH3_THROW(std::runtime_error("Quest override must preserve the disc file length"));

    if (name != "05/quest00_eng.bin" && name != "05/quest01_eng.bin" && name != "05/quest03_eng.bin")
        MH3_THROW(std::runtime_error("Only quest files may be overridden"));

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
        MH3_THROW(std::runtime_error("Missing FST file: " + name));

    return it->second.first;
}

uint32_t DiscImage::FileSize(const std::string& name) const {
    const auto it = fileEntries.find(name);

    if (it == fileEntries.end())
        MH3_THROW(std::runtime_error("Missing FST file: " + name));

    return it->second.second;
}
