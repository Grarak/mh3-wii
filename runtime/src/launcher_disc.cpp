#include "vita_launcher.h"
#include "fatal.h"
#include "disc_image.h"
#include "fs_status.h"
#include "rom_image.h"
#include <algorithm>
#include <fstream>
#include <stdexcept>
#ifdef __vita__
#include <psp2/io/fcntl.h>
#endif

namespace VitaLauncher {
namespace {
// ProcessDisc reads words only within the bytes it has read.
uint32_t Word(const std::vector<uint8_t>& b, size_t at) {
    if (at > b.size() || b.size() - at < 4)
        MH3_THROW(std::runtime_error("ROM metadata is truncated"));

    return uint32_t(b[at]) << 24 | uint32_t(b[at + 1]) << 16 | uint32_t(b[at + 2]) << 8 | b[at + 3];
}

Status Write(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    out.close();

    if (!out)
        return Status::Error("Cannot write prepared ROM metadata");

    return {};
}

Status Write(const std::filesystem::path& path, const std::string& text) {
    return Write(path, std::vector<uint8_t>(text.begin(), text.end()));
}

Status Check(uint64_t count) {
    if (count > 16 * 1024 * 1024)
        return Status::Error("ROM metadata exceeds the memory limit");

    return {};
}

std::string RecordedRom(const std::filesystem::path& output) {
    std::ifstream list((output / "rom.txt").string());
    std::string path;
    std::getline(list, path);
    return path;
}
} // namespace

bool DiscReady(const std::filesystem::path& output) {
    // Opens partition.bin or the disc image named by rom.txt and checks
    // the metadata against it.
    DiscImage disc;
    bool layout = false;
    return disc.Load(output, false).ok() && FsStatus::Exists(output / "disc-layout.txt", layout).ok() && layout;
}

namespace {
Status ImagePath(const std::filesystem::path& iso, std::filesystem::path& image) {
#ifdef __vita__
    // libstdc++'s POSIX absolute() treats the Vita mount prefix as relative.
    image = iso;
    return {};
#else
    return FsStatus::Absolute(iso, image);
#endif
}
} // namespace

bool DiscPreparedFrom(const std::filesystem::path& output, const std::filesystem::path& iso) {
    std::filesystem::path image;
    return ImagePath(iso, image).ok() && RecordedRom(output) == image.string();
}

Status ProcessDisc(const std::filesystem::path& iso, const std::filesystem::path& output, const Progress& progress) {
    std::filesystem::path image;
    MH3_RETURN_IF_ERROR(ImagePath(iso, image));

    if (DiscReady(output)) {
        bool recorded = false;
        MH3_RETURN_IF_ERROR(FsStatus::Exists(output / "rom.txt", recorded));

        if (!recorded || RecordedRom(output) == image.string())
            return {};
    }

    bool published = false, layout = false;
    MH3_RETURN_IF_ERROR(FsStatus::Exists(output, published));

    if (published)
        MH3_RETURN_IF_ERROR(FsStatus::Exists(output / "disc-layout.txt", layout));

    if (published && !layout)
        return Status::Error("Processing output contains unrelated files: " + output.string());

    progress("Reading the disc image", 0, 1);
    std::unique_ptr<RomImage> opened;
    MH3_RETURN_IF_ERROR(RomImage::Open(image, opened));
    const RomImage& source = *opened;
    const auto staging = std::filesystem::path(output.string() + ".processing");
    bool stale = false;

    // Only this launcher's named staging directory is disposable; no input,
    // saves or existing published game directory is ever removed.
    MH3_RETURN_IF_ERROR(FsStatus::Exists(staging, stale));

    if (stale)
        MH3_RETURN_IF_ERROR(FsStatus::RemoveAll(staging));

    MH3_RETURN_IF_ERROR(FsStatus::CreateDirectories(staging / "sys"));
    std::vector<uint8_t> bytes;
    const auto copy = [&](uint64_t at, uint64_t count, const std::filesystem::path& path) {
        MH3_RETURN_IF_ERROR(Check(count));
        MH3_RETURN_IF_ERROR(source.ReadPartition(at, count, bytes));
        return Write(path, bytes);
    };

    std::vector<uint8_t> boot, app, dol;
    MH3_RETURN_IF_ERROR(source.ReadPartition(0, 0x440, boot));
    MH3_RETURN_IF_ERROR(Write(staging / "sys/boot.bin", boot));
    MH3_RETURN_IF_ERROR(copy(0x440, 0x2000, staging / "sys/bi2.bin"));
    MH3_RETURN_IF_ERROR(source.ReadPartition(0x2440, 0x20, app));
    MH3_RETURN_IF_ERROR(copy(0x2440, 0x20 + uint64_t(Word(app, 0x14)) + Word(app, 0x18), staging / "sys/apploader.img"));
    const uint64_t dolAt = uint64_t(Word(boot, 0x420)) * 4;
    MH3_RETURN_IF_ERROR(source.ReadPartition(dolAt, 0x100, dol));
    uint64_t dolSize = 0x100;

    for (unsigned i = 0; i < 18; ++i)
        dolSize = std::max(dolSize, uint64_t(Word(dol, i * 4)) + Word(dol, 0x90 + i * 4));

    MH3_RETURN_IF_ERROR(copy(dolAt, dolSize, staging / "sys/main.dol"));
    const uint64_t fstAt = uint64_t(Word(boot, 0x424)) * 4, fstSize = uint64_t(Word(boot, 0x428)) * 4;

    if (fstSize < 12 || fstSize > 1024 * 1024)
        return Status::Error("Invalid FST size");

    MH3_RETURN_IF_ERROR(copy(fstAt, fstSize, staging / "sys/fst.bin"));
    const uint64_t partition = source.PartitionOffset();
    const auto& header = source.PartitionHeader();
    const auto raw = [&](uint64_t at, uint64_t count, const std::filesystem::path& path) {
        MH3_RETURN_IF_ERROR(Check(count));
        MH3_RETURN_IF_ERROR(source.ReadDisc(at, count, bytes));
        return Write(path, bytes);
    };

    MH3_RETURN_IF_ERROR(raw(partition, 0x2a4, staging / "ticket.bin"));
    MH3_RETURN_IF_ERROR(raw(partition + uint64_t(Word(header, 0x2a8)) * 4, Word(header, 0x2a4), staging / "tmd.bin"));
    MH3_RETURN_IF_ERROR(raw(partition + uint64_t(Word(header, 0x2b0)) * 4, Word(header, 0x2ac), staging / "cert.bin"));
    MH3_RETURN_IF_ERROR(raw(partition + uint64_t(Word(header, 0x2b4)) * 4, 0x18000, staging / "h3.bin"));
    MH3_RETURN_IF_ERROR(Write(staging / "disc-layout.txt", std::string("mh3-partition-v1\n")));

    // The game partition stays in the disc image and is read in place.
    MH3_RETURN_IF_ERROR(Write(staging / "rom.txt", image.string() + "\n" + std::to_string(source.FileBytes()) + "\n"));

    if (!DiscReady(staging))
        return Status::Error("Prepared disc validation failed");

    MH3_RETURN_IF_ERROR(FsStatus::Exists(output, published));

    if (published) {
        bool cache = false;

        // Keep converted assets; they depend on the game's files, not the image's path.
        MH3_RETURN_IF_ERROR(FsStatus::Exists(output / "cache", cache));

        if (cache)
            MH3_RETURN_IF_ERROR(FsStatus::Rename(output / "cache", staging / "cache"));

        MH3_RETURN_IF_ERROR(FsStatus::RemoveAll(output));
    }
#ifdef __vita__
    if (sceIoSync("ux0:", 0) < 0)
        return Status::Error("Cannot sync prepared game data");
#endif
    MH3_RETURN_IF_ERROR(FsStatus::Rename(staging, output));
    progress("Game data ready", 1, 1);
    return {};
}
} // namespace VitaLauncher
