#include "rom_image.h"
#include "fatal.h"
#include <aurora/thread_role.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <stdexcept>
#include <string>
#include "../vita/rom_decoder.h"
#ifdef __vita__
#include "../vita/aligned_io.h"
#endif
#ifdef __vita__
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
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
    File() = default;

    Status Open(const std::filesystem::path& path) {
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
            return Status::Error("Cannot read ROM: " + path.string());
        }

        size = st.st_size;
        return {};
    }

    ~File() { Close(); }

    File(const File&) = delete;

    Status Read(uint64_t at, uint8_t* bytes, size_t count) const {
        if (at > size || count > size - at || at > INT64_MAX)
            return Status::Error("ROM is truncated");

#ifdef MH3_DEVELOPMENT
        const auto start = std::chrono::steady_clock::now();
#endif
        size_t done = 0;

        while (done < count) {
#ifdef __vita__
            const auto n = VitaIo::Pread(fd, bytes + done, count - done, at + done);
#else
            const auto n = pread(fd, bytes + done, count - done, at + done);

            if (n < 0 && errno == EINTR)
                continue;
#endif
            if (n <= 0)
                return Status::Error("ROM read failed");

            done += n;
        }
#ifdef MH3_DEVELOPMENT
        reads.fetch_add(1, std::memory_order_relaxed);
        readNs.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()), std::memory_order_relaxed);
#endif
        return {};
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

// The decoder's read-ahead worker. Vita: on core 3 beside the draw thread
// (core 0 holds the DVD reader, which decodes beside it and waits on it),
// below the default priority so a draw never waits for it; on core 0 without
// core 3.
void PinDecodeWorker() {
#ifdef __vita__
    if (sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0 << 3) < 0)
        sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);

    constexpr int DefaultPriority = 0x10000100; // SCE_KERNEL_DEFAULT_PRIORITY_USER
    sceKernelChangeThreadPriority(sceKernelGetThreadId(), DefaultPriority + 16);
#else
    aurora::pin_current_thread(aurora::ThreadRole::Worker);
#endif
}

uint32_t Word(const std::vector<uint8_t>& b, size_t at) {
    if (at > b.size() || b.size() - at < 4)
        MH3_THROW(std::runtime_error("ROM metadata is truncated"));

    return uint32_t(b[at]) << 24 | uint32_t(b[at + 1]) << 16 | uint32_t(b[at + 2]) << 8 | b[at + 3];
}
} // namespace

// The decoder (rom_decoder.cpp) handles RVZ/WIA decompression and Wii
// title-key/AES decoding. Its read callback keeps full 64-bit source offsets
// on Vita's 32-bit newlib.
class RomImage::Source {
  public:
    File file;
    uint64_t size = 0;

    Status Open(const std::filesystem::path& path) {
        MH3_RETURN_IF_ERROR(file.Open(path));
        size = file.size;
        std::array<uint8_t, 0x100> header{};
        MH3_RETURN_IF_ERROR(file.Read(0, header.data(), header.size()));

        // The decoder treats ISO sectors as encrypted. nodtool's decrypted
        // ISO marks byte 0x61; the bounded sector reader below reads it.
        if (header[0x61] == 1 && std::string(header.begin(), header.begin() + 6) == "RMHP08")
            return {};

        std::array<uint8_t, 512> error{};
        decoder = mh3_rom_open(
            &file, file.size,
            [](void* context, uint64_t at, uint8_t* bytes, size_t count) -> intptr_t {
                return static_cast<File*>(context)->Read(at, bytes, count).ok() ? intptr_t(count) : -1;
            },
            error.data(), error.size());
        if (!decoder)
            return Status::Error(reinterpret_cast<const char*>(error.data()));

        // A worker decodes the next RVZ group of a long read beside the reader.
        mh3_rom_read_ahead(decoder, PinDecodeWorker);
        size = mh3_rom_size(decoder, 0);
        return {};
    }

    ~Source() {
        mh3_rom_close(decoder);
    }

    bool Decoded() const {
        return decoder != nullptr;
    }

    uint64_t FlatBytes() const {
        return mh3_rom_size(decoder, 1);
    }

    Status Read(uint64_t at, uint8_t* bytes, size_t count, bool flat) const {
        if (decoder) {
            std::array<uint8_t, 512> error{};

            if (mh3_rom_read(decoder, flat, at, bytes, count, error.data(), error.size()) < 0)
                return Status::Error(reinterpret_cast<const char*>(error.data()));

            return {};
        }

        return file.Read(at, bytes, count);
    }

  private:
    void* decoder = nullptr;
};

RomImage::RomImage() = default;
RomImage::~RomImage() = default;

Status RomImage::Open(const std::filesystem::path& path, std::unique_ptr<RomImage>& image) {
    std::unique_ptr<RomImage> opened(new RomImage);
    MH3_RETURN_IF_ERROR(opened->Load(path));
    image = std::move(opened);
    return {};
}

// Each vector Word reads below has the size that was read, so the words are
// always there.
Status RomImage::Load(const std::filesystem::path& path) {
    source = std::make_unique<Source>();
    MH3_RETURN_IF_ERROR(source->Open(path));
    std::vector<uint8_t> discHeader;
    MH3_RETURN_IF_ERROR(ReadDisc(0, 0x100, discHeader));

    if (std::string(discHeader.begin(), discHeader.begin() + 6) != "RMHP08" || Word(discHeader, 0x18) != 0x5d1c9ea3)
        return Status::Error("Expected the European RMHP08 Wii disc image.");

    std::vector<uint8_t> groups;
    MH3_RETURN_IF_ERROR(ReadDisc(0x40000, 32, groups));
    unsigned found = 0;

    for (unsigned group = 0; group < 4; ++group) {
        const uint32_t count = Word(groups, group * 8);
        const uint64_t table = uint64_t(Word(groups, group * 8 + 4)) * 4;

        if (count > 256)
            return Status::Error("Invalid Wii partition table");

        std::vector<uint8_t> entries;
        MH3_RETURN_IF_ERROR(ReadDisc(table, count * 8, entries));

        for (unsigned i = 0; i < count; ++i)
            if (Word(entries, i * 8 + 4) == 0) {
                partition = uint64_t(Word(entries, i * 8)) * 4;
                ++found;
            }
    }

    if (found != 1)
        return Status::Error("Expected exactly one game partition");

    MH3_RETURN_IF_ERROR(ReadDisc(partition, 0x2c0, partitionHeader));
    dataStart = partition + uint64_t(Word(partitionHeader, 0x2b8)) * 4;
    const uint64_t size = uint64_t(Word(partitionHeader, 0x2bc)) * 4;

    if (!size || size % 0x8000 || dataStart > source->size || size > source->size - dataStart)
        return Status::Error("Invalid Wii partition extent");

    flatBytes = size / 0x8000 * 0x7c00;

    if (source->Decoded() && flatBytes != source->FlatBytes())
        return Status::Error("Decoded partition extent differs from its header");

    std::vector<uint8_t> boot;
    MH3_RETURN_IF_ERROR(ReadPartition(0, 0x440, boot));

    if (std::string(boot.begin(), boot.begin() + 6) != "RMHP08" || Word(boot, 0x18) != 0x5d1c9ea3)
        return Status::Error("ISO partition is encrypted. Convert with nodtool convert --decrypt before transferring.");

    return {};
}

uint64_t RomImage::FileBytes() const {
    return source->file.size;
}

Status RomImage::ReadPartition(uint64_t at, uint8_t* bytes, size_t count) const {
    if (at > flatBytes || count > flatBytes - at)
        return Status::Error("Read beyond the game partition");

    std::lock_guard guard(lock);

    if (source->Decoded())
        return source->Read(at, bytes, count, true);

    // Decrypted ISO: skip each sector's 0x400-byte hash block.
    size_t done = 0;

    while (done < count) {
        const size_t amount = std::min<size_t>(count - done, 0x7c00 - at % 0x7c00);
        MH3_RETURN_IF_ERROR(source->Read(dataStart + at / 0x7c00 * 0x8000 + 0x400 + at % 0x7c00, bytes + done, amount, false));
        at += amount;
        done += amount;
    }

    return {};
}

Status RomImage::ReadPartition(uint64_t at, size_t count, std::vector<uint8_t>& bytes) const {
    bytes.resize(count);
    return ReadPartition(at, bytes.data(), count);
}

Status RomImage::ReadDisc(uint64_t at, size_t count, std::vector<uint8_t>& bytes) const {
    bytes.resize(count);
    std::lock_guard guard(lock);
    return source->Read(at, bytes.data(), count, false);
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
                CheckStatus(file.Read(at + done, buffer.data, size));
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

        CheckStatus(source->Decoded() ? source->Read(flat, heap.data(), kChunk, true) : file.Read(dataStart + flat / 0x7c00 * 0x8000, heap.data(), kChunk));

        worst = std::max(worst, ms(Clock::now() - read));
    }

    const double total = ms(Clock::now() - start), io = (file.readNs.load() - readNs) / 1e6;
    std::fprintf(stderr, "MH3 disc benchmark: decoded 512 KiB reads: %.1f MiB/s, %.2f ms avg %.2f max; image reads %.1f ms (%.0f%%, %llu reads)\n",
                 kCase / 1048576.0 / (total / 1000.0), total / double(kCase / kChunk), worst, io, 100.0 * io / total,
                 static_cast<unsigned long long>(file.reads.load() - reads));
}
#endif
