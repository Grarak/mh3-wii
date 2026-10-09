#include "rom_image.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <stdexcept>
#include <string>
#ifdef MH3_HAS_ROM_DECODER
#include "../vita/rom_decoder.h"
#endif
#ifdef __vita__
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/sysmem.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
class File {
  public:
    uint64_t size = 0;
    explicit File(const std::filesystem::path& path) {
#ifdef __vita__
        fd = sceIoOpen(path.string().c_str(), SCE_O_RDONLY, 0);
        SceIoStat st{};
        const int result = fd < 0 ? -1 : sceIoGetstatByFd(fd, &st);
#else
        fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        struct stat st {};
        const int result = fd < 0 ? -1 : fstat(fd, &st);
#endif
        if (result < 0) {
            Close();
            throw std::runtime_error("Cannot read ROM: " + path.string());
        }

        size = st.st_size;
    }

    ~File() { Close(); }

    File(const File&) = delete;

    void Read(uint64_t at, uint8_t* bytes, size_t count) const {
        if (at > size || count > size - at || at > INT64_MAX)
            throw std::runtime_error("ROM is truncated");

#ifdef MH3_DEVELOPMENT
        const auto start = std::chrono::steady_clock::now();
#endif
        size_t done = 0;

        while (done < count) {
#ifdef __vita__
            const auto n = sceIoPread(fd, bytes + done, count - done, at + done);
#else
            const auto n = pread(fd, bytes + done, count - done, at + done);

            if (n < 0 && errno == EINTR)
                continue;
#endif
            if (n <= 0)
                throw std::runtime_error("ROM read failed");

            done += n;
        }
#ifdef MH3_DEVELOPMENT
        reads.fetch_add(1, std::memory_order_relaxed);
        readNs.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()), std::memory_order_relaxed);
#endif
    }

#ifdef MH3_DEVELOPMENT
    // Development: positioned reads and their time (MH3_PROFILE=disc).
    mutable std::atomic<uint64_t> reads{0}, readNs{0};
#endif

  private:
    int fd = -1;
    void Close() {
        if (fd < 0)
            return;
#ifdef __vita__
        sceIoClose(fd);
#else
        ::close(fd);
#endif
        fd = -1;
    }
};

uint32_t Word(const std::vector<uint8_t>& b, size_t at) {
    if (at > b.size() || b.size() - at < 4)
        throw std::runtime_error("ROM metadata is truncated");

    return uint32_t(b[at]) << 24 | uint32_t(b[at + 1]) << 16 | uint32_t(b[at + 2]) << 8 | b[at + 3];
}
} // namespace

// nod handles RVZ/WIA decompression and Wii title-key/AES decoding. Native
// callbacks retain full 64-bit source offsets on Vita's 32-bit newlib.
class RomImage::Source {
  public:
    File file;
    uint64_t size = 0;
    explicit Source(const std::filesystem::path& path) : file(path) {
        size = file.size;
#ifdef MH3_HAS_ROM_DECODER
        std::array<uint8_t, 0x100> header{};
        file.Read(0, header.data(), header.size());

        // nod 1.4 treats plain ISO sectors as encrypted. nodtool's decrypted
        // ISO marks byte 0x61; retain the bounded native sector reader for it.
        if (header[0x61] == 1 && std::string(header.begin(), header.begin() + 6) == "RMHP08")
            return;

        std::array<uint8_t, 512> error{};
        decoder = mh3_rom_open(
            &file, file.size,
            [](void* context, uint64_t at, uint8_t* bytes, size_t count) -> intptr_t {
                try {
                    static_cast<File*>(context)->Read(at, bytes, count);
                    return count;
                } catch (...) {
                    return -1;
                }
            },
            error.data(), error.size());
        if (!decoder)
            throw std::runtime_error(reinterpret_cast<const char*>(error.data()));

        size = mh3_rom_size(decoder, 0);
#endif
    }

    ~Source() {
#ifdef MH3_HAS_ROM_DECODER
        mh3_rom_close(decoder);
#endif
    }

    bool Decoded() const {
#ifdef MH3_HAS_ROM_DECODER
        return decoder != nullptr;
#else
        return false;
#endif
    }

    uint64_t FlatBytes() const {
#ifdef MH3_HAS_ROM_DECODER
        return mh3_rom_size(decoder, 1);
#else
        return 0;
#endif
    }

    void Read(uint64_t at, uint8_t* bytes, size_t count, bool flat) const {
#ifdef MH3_HAS_ROM_DECODER
        if (decoder) {
            std::array<uint8_t, 512> error{};

            if (mh3_rom_read(decoder, flat, at, bytes, count, error.data(), error.size()) < 0)
                throw std::runtime_error(reinterpret_cast<const char*>(error.data()));

            return;
        }
#endif
        (void)flat;
        file.Read(at, bytes, count);
    }

  private:
#ifdef MH3_HAS_ROM_DECODER
    void* decoder = nullptr;
#endif
};

RomImage::~RomImage() = default;
RomImage::RomImage(const std::filesystem::path& path) : source(std::make_unique<Source>(path)) {
    const auto discHeader = ReadDisc(0, 0x100);

    if (std::string(discHeader.begin(), discHeader.begin() + 6) != "RMHP08" || Word(discHeader, 0x18) != 0x5d1c9ea3)
        throw std::runtime_error("Expected the European RMHP08 Wii disc image.");

    const auto groups = ReadDisc(0x40000, 32);
    unsigned found = 0;

    for (unsigned group = 0; group < 4; ++group) {
        const uint32_t count = Word(groups, group * 8);
        const uint64_t table = uint64_t(Word(groups, group * 8 + 4)) * 4;

        if (count > 256)
            throw std::runtime_error("Invalid Wii partition table");

        const auto entries = ReadDisc(table, count * 8);

        for (unsigned i = 0; i < count; ++i)
            if (Word(entries, i * 8 + 4) == 0) {
                partition = uint64_t(Word(entries, i * 8)) * 4;
                ++found;
            }
    }

    if (found != 1)
        throw std::runtime_error("Expected exactly one game partition");

    partitionHeader = ReadDisc(partition, 0x2c0);
    dataStart = partition + uint64_t(Word(partitionHeader, 0x2b8)) * 4;
    const uint64_t size = uint64_t(Word(partitionHeader, 0x2bc)) * 4;

    if (!size || size % 0x8000 || dataStart > source->size || size > source->size - dataStart)
        throw std::runtime_error("Invalid Wii partition extent");

    flatBytes = size / 0x8000 * 0x7c00;

    if (source->Decoded() && flatBytes != source->FlatBytes())
        throw std::runtime_error("Decoded partition extent differs from its header");

    const auto boot = ReadPartition(0, 0x440);

    if (std::string(boot.begin(), boot.begin() + 6) != "RMHP08" || Word(boot, 0x18) != 0x5d1c9ea3)
        throw std::runtime_error("ISO partition is encrypted. Convert with nodtool convert --decrypt before transferring.");
}

uint64_t RomImage::FileBytes() const {
    return source->file.size;
}

void RomImage::ReadPartition(uint64_t at, uint8_t* bytes, size_t count) const {
    if (at > flatBytes || count > flatBytes - at)
        throw std::runtime_error("Read beyond the game partition");

    std::lock_guard guard(lock);

    if (source->Decoded()) {
        source->Read(at, bytes, count, true);
        return;
    }

    // Decrypted ISO: skip each sector's 0x400-byte hash block.
    size_t done = 0;

    while (done < count) {
        const size_t amount = std::min<size_t>(count - done, 0x7c00 - at % 0x7c00);
        source->Read(dataStart + at / 0x7c00 * 0x8000 + 0x400 + at % 0x7c00, bytes + done, amount, false);
        at += amount;
        done += amount;
    }
}

std::vector<uint8_t> RomImage::ReadPartition(uint64_t at, size_t count) const {
    std::vector<uint8_t> b(count);
    ReadPartition(at, b.data(), count);
    return b;
}

std::vector<uint8_t> RomImage::ReadDisc(uint64_t at, size_t count) const {
    std::vector<uint8_t> b(count);
    std::lock_guard guard(lock);
    source->Read(at, b.data(), count, false);
    return b;
}

#ifdef MH3_DEVELOPMENT
// Each case reads its own 8 MiB of the image, so no read is served from a
// cache: plain reads by size into ordinary and (Vita) physically contiguous
// memory, then the decoder's 512 KiB partition reads (a DVD library chunk) of
// game data with the share of their time spent in the image's reads.
void RomImage::Benchmark(uint64_t sample) const {
    using Clock = std::chrono::steady_clock;
    constexpr size_t kCase = 8u << 20, kLargest = 2u << 20;
    const size_t sizes[] = {32u << 10, 128u << 10, 512u << 10, kLargest};
    const auto ms = [](Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
    std::lock_guard guard(lock);
    File& file = source->file;
    struct Buffer {
        const char* name;
        uint8_t* data;
    };

    std::vector<uint8_t> heap(kLargest);
    std::vector<Buffer> buffers{{"heap", heap.data()}};
#ifdef __vita__
    const SceUID block = sceKernelAllocMemBlock("MH3 disc benchmark", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW, kLargest, nullptr);
    void* base = nullptr;

    if (block >= 0 && sceKernelGetMemBlockBase(block, &base) >= 0)
        buffers.push_back({"phycont", static_cast<uint8_t*>(base)});
    else
        std::fprintf(stderr, "MH3 disc benchmark: no phycont block (%08x)\n", unsigned(block));
#endif
    uint64_t at = file.size / 4 & ~uint64_t(0xFFFF);

    for (const auto& buffer : buffers)
        for (const size_t size : sizes) {
            if (at + kCase > file.size)
                break;

            const auto start = Clock::now();
            double worst = 0;

            for (size_t done = 0; done < kCase; done += size) {
                const auto read = Clock::now();
                file.Read(at + done, buffer.data, size);
                worst = std::max(worst, ms(Clock::now() - read));
            }

            const double total = ms(Clock::now() - start);
            std::fprintf(stderr, "MH3 disc benchmark: %s reads of %u KiB: %.1f MiB/s, %.2f ms avg %.2f max\n", buffer.name, unsigned(size >> 10),
                         kCase / 1048576.0 / (total / 1000.0), total / double(kCase / size), worst);
            at += kCase;
        }
#ifdef __vita__
    if (block >= 0)
        sceKernelFreeMemBlock(block);
#endif
    constexpr size_t kChunk = 512u << 10;
    const uint64_t reads = file.reads.load(), readNs = file.readNs.load();
    const auto start = Clock::now();
    double worst = 0;

    for (uint64_t flat = sample, end = flat + kCase; flat < end && end <= flatBytes; flat += kChunk) {
        const auto read = Clock::now();

        if (source->Decoded())
            source->Read(flat, heap.data(), kChunk, true);
        else
            file.Read(dataStart + flat / 0x7c00 * 0x8000, heap.data(), kChunk);

        worst = std::max(worst, ms(Clock::now() - read));
    }

    const double total = ms(Clock::now() - start), io = (file.readNs.load() - readNs) / 1e6;
    std::fprintf(stderr, "MH3 disc benchmark: decoded 512 KiB reads: %.1f MiB/s, %.2f ms avg %.2f max; image reads %.1f ms (%.0f%%, %llu reads)\n",
                 kCase / 1048576.0 / (total / 1000.0), total / double(kCase / kChunk), worst, io, 100.0 * io / total,
                 static_cast<unsigned long long>(file.reads.load() - reads));
}
#endif
