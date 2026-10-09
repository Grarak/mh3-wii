#include "vita_launcher.h"
#include "disc_image.h"
#include "rom_image.h"
#include <algorithm>
#include <fstream>
#include <stdexcept>
#ifdef __vita__
#include <psp2/io/fcntl.h>
#endif

namespace VitaLauncher {
namespace {
uint32_t Word(const std::vector<uint8_t>& b, size_t at) {
    if (at > b.size() || b.size() - at < 4)
        throw std::runtime_error("ROM metadata is truncated");
    return uint32_t(b[at]) << 24 | uint32_t(b[at + 1]) << 16 | uint32_t(b[at + 2]) << 8 | b[at + 3];
}
void Write(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    out.close();
    if (!out)
        throw std::runtime_error("Cannot write prepared ROM metadata");
}
void Write(const std::filesystem::path& path, const std::string& text) {
    Write(path, std::vector<uint8_t>(text.begin(), text.end()));
}
void Check(uint64_t count) {
    if (count > 16 * 1024 * 1024)
        throw std::runtime_error("ROM metadata exceeds the memory limit");
}
std::string RecordedRom(const std::filesystem::path& output) {
    std::ifstream list((output / "rom.txt").string());
    std::string path;
    std::getline(list, path);
    return path;
}
} // namespace
bool DiscReady(const std::filesystem::path& output) {
    try {
        // Opens partition.bin or the disc image named by rom.txt and checks
        // the metadata against it.
        DiscImage disc(output, false);
        return std::filesystem::exists(output / "disc-layout.txt");
    } catch (const std::exception&) {
        return false;
    }
}
void ProcessDisc(const std::filesystem::path& iso, const std::filesystem::path& output, const Progress& progress) {
    const auto image = std::filesystem::absolute(iso);
    if (DiscReady(output) && (!std::filesystem::exists(output / "rom.txt") || RecordedRom(output) == image.string()))
        return;
    if (std::filesystem::exists(output) && !std::filesystem::exists(output / "disc-layout.txt"))
        throw std::runtime_error("Processing output contains unrelated files: " + output.string());
    progress("Reading the disc image", 0, 1);
    RomImage source(image);
    const auto staging = std::filesystem::path(output.string() + ".processing");
    // Only this launcher's named staging directory is disposable; no input,
    // saves or existing published game directory is ever removed.
    if (std::filesystem::exists(staging))
        std::filesystem::remove_all(staging);
    std::filesystem::create_directories(staging / "sys");
    const auto copy = [&](uint64_t at, uint64_t count, const std::filesystem::path& path) {
        Check(count);
        Write(path, source.ReadPartition(at, count));
    };
    const auto boot = source.ReadPartition(0, 0x440);
    Write(staging / "sys/boot.bin", boot);
    copy(0x440, 0x2000, staging / "sys/bi2.bin");
    const auto app = source.ReadPartition(0x2440, 0x20);
    copy(0x2440, 0x20 + uint64_t(Word(app, 0x14)) + Word(app, 0x18), staging / "sys/apploader.img");
    const uint64_t dolAt = uint64_t(Word(boot, 0x420)) * 4;
    const auto dol = source.ReadPartition(dolAt, 0x100);
    uint64_t dolSize = 0x100;
    for (unsigned i = 0; i < 18; ++i)
        dolSize = std::max(dolSize, uint64_t(Word(dol, i * 4)) + Word(dol, 0x90 + i * 4));
    copy(dolAt, dolSize, staging / "sys/main.dol");
    const uint64_t fstAt = uint64_t(Word(boot, 0x424)) * 4, fstSize = uint64_t(Word(boot, 0x428)) * 4;
    if (fstSize < 12 || fstSize > 1024 * 1024)
        throw std::runtime_error("Invalid FST size");
    copy(fstAt, fstSize, staging / "sys/fst.bin");
    const uint64_t partition = source.PartitionOffset();
    const auto& header = source.PartitionHeader();
    const auto raw = [&](uint64_t at, uint64_t count, const std::filesystem::path& path) {
        Check(count);
        Write(path, source.ReadDisc(at, count));
    };
    raw(partition, 0x2a4, staging / "ticket.bin");
    raw(partition + uint64_t(Word(header, 0x2a8)) * 4, Word(header, 0x2a4), staging / "tmd.bin");
    raw(partition + uint64_t(Word(header, 0x2b0)) * 4, Word(header, 0x2ac), staging / "cert.bin");
    raw(partition + uint64_t(Word(header, 0x2b4)) * 4, 0x18000, staging / "h3.bin");
    Write(staging / "disc-layout.txt", std::string("mh3-partition-v1\n"));
    // The game partition stays in the disc image and is read in place.
    Write(staging / "rom.txt", image.string() + "\n" + std::to_string(source.FileBytes()) + "\n");
    if (!DiscReady(staging))
        throw std::runtime_error("Prepared disc validation failed");
    if (std::filesystem::exists(output)) {
        // Keep converted assets; they depend on the game's files, not the image's path.
        if (std::filesystem::exists(output / "cache"))
            std::filesystem::rename(output / "cache", staging / "cache");
        std::filesystem::remove_all(output);
    }
#ifdef __vita__
    if (sceIoSync("ux0:", 0) < 0)
        throw std::runtime_error("Cannot sync prepared game data");
#endif
    std::filesystem::rename(staging, output);
    progress("Game data ready", 1, 1);
}
} // namespace VitaLauncher
