// Language pack files (language_pack.h).
#include "language_pack.h"
#include "disc_image.h"
#include "fatal.h"
#include "fs_status.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace LanguagePack {
namespace {
// 2: a replaced BRRES has the European resources the Japanese one lacks.
// Packs of 1 still load; the launcher imports them again (Outdated).
constexpr char kMagic[8]{'M', 'H', '3', 'L', 'A', 'N', 'G', '2'}, kOldMagic[8]{'M', 'H', '3', 'L', 'A', 'N', 'G', '1'};

// Past the end of the disc's partition; FST offsets are words, and stay
// positive.
constexpr uint64_t kVirtualBase = 0x120000000ull, kVirtualPack = 256u << 20, kVirtualFile = 16u << 20;
constexpr uint32_t kVirtualPacks = 8;

class Writer {
  public:
    std::vector<uint8_t> bytes;

    void U32(uint32_t value) {
        for (unsigned i = 0; i < 4; ++i)
            bytes.push_back(uint8_t(value >> (i * 8)));
    }

    void Text(const std::string& text) {
        U32(uint32_t(text.size()));
        bytes.insert(bytes.end(), text.begin(), text.end());
    }

    void Blob(const std::vector<uint8_t>& blob) {
        U32(uint32_t(blob.size()));
        bytes.insert(bytes.end(), blob.begin(), blob.end());
    }
};

class Reader {
  public:
    explicit Reader(const std::vector<uint8_t>& bytes) : data(bytes) {}

    bool ok() const { return good; }

    uint32_t U32() {
        if (at + 4 > data.size()) {
            good = false;
            return 0;
        }

        const uint32_t value = uint32_t(data[at]) | uint32_t(data[at + 1]) << 8 | uint32_t(data[at + 2]) << 16 | uint32_t(data[at + 3]) << 24;
        at += 4;
        return value;
    }

    uint32_t Count(uint32_t itemBytes) {
        const uint32_t count = U32();

        if (uint64_t(count) * itemBytes > data.size() - at) {
            good = false;
            return 0;
        }

        return count;
    }

    std::string Text() {
        const uint32_t count = Count(1);
        std::string text(data.begin() + at, data.begin() + at + count);
        at += count;
        return text;
    }

    std::vector<uint8_t> Blob() {
        const uint32_t count = Count(1);
        std::vector<uint8_t> blob(data.begin() + at, data.begin() + at + count);
        at += count;
        return blob;
    }

  private:
    const std::vector<uint8_t>& data;
    size_t at = 0;
    bool good = true;
};

} // namespace

std::filesystem::path PackPath(const std::filesystem::path& gameData, const std::string& code) {
    return Directory(gameData) / (code + ".mh3lang");
}

bool Outdated(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    char magic[8]{};
    file.read(magic, 8);

    if (!file || std::memcmp(magic, kOldMagic, 8))
        return false;

    file.close();
    Pack pack;
    return Read(path, pack, false).ok() && !pack.files.empty();
}

// Only the names and file table unless `whole`.
Status Read(const std::filesystem::path& path, Pack& out, bool whole) {
    std::ifstream file(path, std::ios::binary);
    char magic[8]{};
    uint32_t size = 0;
    file.read(magic, 8);
    file.read(reinterpret_cast<char*>(&size), 4);

    if (!file || (std::memcmp(magic, kMagic, 8) && std::memcmp(magic, kOldMagic, 8)) || size > 64u << 20)
        return Status::Error("Not a language pack: " + path.string());

    std::vector<uint8_t> header(whole ? size : std::min<uint32_t>(size, 4096));
    file.read(reinterpret_cast<char*>(header.data()), header.size());

    if (!file)
        return Status::Error("Truncated language pack: " + path.string());

    Reader in(header);
    auto& c = out.contents;
    c.code = in.Text();
    c.name = in.Text();
    out.files.resize(in.Count(16));

    for (auto& f : out.files) {
        f.name = in.Text();
        const uint32_t low = in.U32(), high = in.U32();
        f.at = uint64_t(high) << 32 | low;
        f.bytes = in.U32();
    }

    out.path = path;

    if (!whole)
        return in.ok() ? Status{} : Status::Error("Damaged language pack: " + path.string());

    c.text = in.Blob();
    c.references.resize(in.Count(16));

    for (auto& r : c.references)
        r = {in.U32(), in.U32(), in.U32(), in.U32()};

    c.fields.resize(in.Count(24));

    for (auto& f : c.fields)
        f = {in.U32(), in.U32(), in.U32(), in.U32(), in.U32(), in.U32()};

    c.glyphs.resize(in.Count(4));

    for (auto& g : c.glyphs) {
        g.code = uint16_t(in.U32());
        g.narrow = uint16_t(in.U32());
    }

    c.atlasWidth = in.U32();
    c.atlasHeight = in.U32();
    c.atlas = in.Blob();
    return in.ok() ? Status{} : Status::Error("Damaged language pack: " + path.string());
}

namespace {
// The replacement files' bytes follow this part: the disc reader reads them
// from the pack in place.
std::vector<uint8_t> Header(const Contents& c, uint64_t filesAt) {
    Writer out;
    out.Text(c.code);
    out.Text(c.name);
    out.U32(uint32_t(c.replacements.size()));
    uint64_t at = filesAt;

    for (const auto& f : c.replacements) {
        out.Text(f.name);
        out.U32(uint32_t(at));
        out.U32(uint32_t(at >> 32));
        out.U32(uint32_t(f.bytes.size()));
        at += f.bytes.size();
    }

    out.Blob(c.text);
    out.U32(uint32_t(c.references.size()));

    for (const auto& r : c.references)
        for (const uint32_t value : {r.file, r.entry, r.word, r.text})
            out.U32(value);

    out.U32(uint32_t(c.fields.size()));

    for (const auto& f : c.fields)
        for (const uint32_t value : {f.file, f.entry, f.offset, f.bytes, f.english, f.text})
            out.U32(value);

    out.U32(uint32_t(c.glyphs.size()));

    for (const auto& g : c.glyphs) {
        out.U32(g.code);
        out.U32(g.narrow);
    }

    out.U32(c.atlasWidth);
    out.U32(c.atlasHeight);
    out.Blob(c.atlas);
    return out.bytes;
}

} // namespace

// MH3_LANGUAGE_DIR puts it elsewhere (a devkit's host0:).
std::filesystem::path Directory(const std::filesystem::path& gameData) {
    const char* directory = std::getenv("MH3_LANGUAGE_DIR");
    return directory && *directory ? std::filesystem::path(directory) : gameData / "languages";
}

std::vector<Listing> Installed(const std::filesystem::path& gameData) {
    std::vector<Listing> list;
    std::error_code error;
    std::filesystem::directory_iterator entries(Directory(gameData), error);

    if (error)
        return list;

    for (; entries != std::filesystem::directory_iterator(); entries.increment(error)) {
        if (entries->path().extension() != ".mh3lang")
            continue;

        Pack read;

        if (Read(entries->path(), read, false).ok())
            list.push_back({read.contents.code, read.contents.name});
    }

    std::sort(list.begin(), list.end(), [](const Listing& a, const Listing& b) { return a.name < b.name; });
    return list;
}

// Each pack's files have their own offsets, which the asset cache's converted
// copies are named by.
uint64_t VirtualOffset(const std::string& code, size_t index) {
    return kVirtualBase + TextHash(code.c_str()) % kVirtualPacks * kVirtualPack + uint64_t(index) * kVirtualFile;
}

std::vector<DiscFile> RegisterFiles(DiscImage& disc, const std::filesystem::path& gameData) {
    std::vector<DiscFile> files;

    for (const auto& listing : Installed(gameData)) {
        Pack read;
        CheckStatus(Read(PackPath(gameData, listing.code), read, false));

        for (size_t i = 0; i < read.files.size(); ++i) {
            const uint64_t offset = VirtualOffset(listing.code, i);
            CheckStatus(disc.AddVirtualFile(offset, read.files[i].bytes, read.path, read.files[i].at));
            files.push_back({read.files[i].name, offset, read.files[i].bytes});
        }
    }

    return files;
}

Status Write(const std::filesystem::path& gameData, const Contents& c) {
    MH3_RETURN_IF_ERROR(FsStatus::CreateDirectories(Directory(gameData)));
    const auto path = PackPath(gameData, c.code);
    const auto temporary = std::filesystem::path(path.string() + ".tmp");

    // Twice: the header's size does not depend on the offsets it holds.
    const uint32_t size = uint32_t(Header(c, 0).size());
    const uint64_t filesAt = 12 + uint64_t(size);
    const auto header = Header(c, filesAt);
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(kMagic, 8);
        out.write(reinterpret_cast<const char*>(&size), 4);
        out.write(reinterpret_cast<const char*>(header.data()), header.size());

        for (const auto& f : c.replacements)
            out.write(reinterpret_cast<const char*>(f.bytes.data()), f.bytes.size());

        out.close();

        if (!out)
            return Status::Error("Cannot write the language pack (not enough space?)");
    }

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    return FsStatus::Rename(temporary, path);
}

uint32_t TextHash(const char* text) {
    uint32_t hash = 2166136261u;

    for (; *text; ++text)
        hash = (hash ^ uint8_t(*text)) * 16777619u;

    return hash;
}
} // namespace LanguagePack
