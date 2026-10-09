// Startup conversion of external disc textures. The swizzle/CMPR routines
// mirror the verified host packer in scripts/vita_texture_pack.cpp.
#include "vita_launcher.h"
#include "disc_image.h"
#include "launcher_texture_inventory.h"
#include "launcher_convert.h"
#include "gfx/texture_convert.hpp"
#include "gfx/native_texture_pack.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#ifdef __vita__
#include <psp2/io/fcntl.h>
#endif

namespace VitaLauncher {
namespace {
using Record = aurora::gfx::NativeTextureRecord;
struct Source : TextureInventory::Texture {
    std::string path;
};

uint16_t be16(const uint8_t* p) {
    return uint16_t(p[0]) << 8 | p[1];
}

uint8_t reverse_pairs(uint8_t value) {
    return (value >> 6) | ((value >> 2) & 12) | ((value << 2) & 48) | (value << 6);
}

size_t morton(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    size_t index = 0;
    unsigned shift = 0;

    for (unsigned bit = 0; (1u << bit) < std::max(width, height); ++bit) {
        if ((1u << bit) < height)
            index |= size_t((y >> bit) & 1) << shift++;

        if ((1u << bit) < width)
            index |= size_t((x >> bit) & 1) << shift++;
    }

    return index;
}

bool has_transparency(const TextureInventory::Texture& s, const uint8_t* data) {
    uint32_t w = s.width, h = s.height;

    for (unsigned mip = 0; mip < s.mips; ++mip) {
        for (unsigned yy = 0; yy < h; yy += 8)
            for (unsigned xx = 0; xx < w; xx += 8)
                for (unsigned yb = 0; yb < 8; yb += 4)
                    for (unsigned xb = 0; xb < 8; xb += 4, data += 8) {
                        if (be16(data) > be16(data + 2))
                            continue;

                        for (unsigned y = 0; y < 4 && yy + yb + y < h; ++y)
                            for (unsigned x = 0; x < 4 && xx + xb + x < w; ++x)
                                if (((data[4 + y] >> (6 - 2 * x)) & 3) == 3)
                                    return true;
                    }
        w = std::max(w / 2, 1u);
        h = std::max(h / 2, 1u);
    }

    return false;
}

std::vector<uint8_t> compressed(const TextureInventory::Texture& s, const uint8_t* data, bool alpha) {
    std::vector<uint8_t> output;
    const unsigned blockBytes = alpha ? 16 : 8;
    uint32_t w = s.width, h = s.height;

    for (unsigned mip = 0; mip < s.mips; ++mip) {
        const uint32_t bw = std::max(w / 4, 1u), bh = std::max(h / 4, 1u);
        const size_t start = output.size();
        output.resize(start + size_t(bw) * bh * blockBytes);

        for (unsigned yy = 0; yy < h; yy += 8)
            for (unsigned xx = 0; xx < w; xx += 8)
                for (unsigned yb = 0; yb < 8; yb += 4)
                    for (unsigned xb = 0; xb < 8; xb += 4, data += 8) {
                        if (xx + xb >= w || yy + yb >= h)
                            continue;

                        auto* dest = output.data() + start + morton((xx + xb) / 4, (yy + yb) / 4, bw, bh) * blockBytes;
                        const bool threeColor = be16(data) <= be16(data + 2);

                        if (alpha) {
                            // BC3 keeps CMPR's transparent texels' midpoint RGB. SGX uses
                            // the three-colour endpoint rule for BC3 too (hardware guide).
                            uint64_t alphaBits = 0;

                            for (unsigned y = 0; y < 4; ++y)
                                for (unsigned x = 0; x < 4; ++x)
                                    if (threeColor && ((data[4 + y] >> (6 - 2 * x)) & 3) == 3)
                                        alphaBits |= uint64_t(1) << (3 * (4 * y + x));
                            dest[0] = 255;
                            dest[1] = 0;

                            for (unsigned i = 0; i < 6; ++i)
                                dest[2 + i] = alphaBits >> (8 * i);

                            dest += 8;
                        }

                        dest[0] = data[1];
                        dest[1] = data[0];
                        dest[2] = data[3];
                        dest[3] = data[2];

                        for (unsigned y = 0; y < 4; ++y) {
                            uint8_t row = data[4 + y];

                            if (alpha && threeColor)
                                for (unsigned x = 0; x < 4; ++x) {
                                    const unsigned shift = 6 - x * 2;

                                    if (((row >> shift) & 3) == 3)
                                        row &= ~(1u << shift);
                                }
                            dest[4 + y] = reverse_pairs(row);
                        }
                    }
        w = std::max(w / 2, 1u);
        h = std::max(h / 2, 1u);
    }

    return output;
}

std::vector<uint8_t> linear(const TextureInventory::Texture& s, const uint8_t* data) {
    std::vector<uint8_t> output;

    for (unsigned mip = 0; mip < s.mips; ++mip) {
        const auto w = std::max(s.width >> mip, 1u), h = std::max(s.height >> mip, 1u);
        const auto pitch = (w + 7) & ~7u;
        const auto rows = mip + 1 < s.mips ? std::max(std::bit_ceil(s.height) >> mip, 1u) : h;
        const auto storedWidth = mip + 1 < s.mips ? std::max(std::bit_ceil(s.width) >> mip, 8u) : pitch;
        const auto offset = output.size();
        output.resize(offset + size_t(storedWidth) * rows * 4);

        for (unsigned y = 0; y < h; ++y)
            std::memcpy(output.data() + offset + size_t(y) * pitch * 4, data + size_t(y) * w * 4, size_t(w) * 4);

        data += size_t(w) * h * 4;
    }

    return output;
}

void Little32(std::ostream& out, uint32_t value) {
    const char bytes[] = {char(value), char(value >> 8), char(value >> 16), char(value >> 24)};
    out.write(bytes, 4);
}

void Little64(std::ostream& out, uint64_t value) {
    Little32(out, value);
    Little32(out, value >> 32);
}

void Publish(const std::filesystem::path& temporary, const std::filesystem::path& destination) {
    // Keep any old generated pack recoverable when the native rename cannot
    // replace an existing destination. A missing payload is never considered ready.
    if (std::filesystem::exists(destination)) {
        auto backup = std::filesystem::path(destination.string() + ".previous");
        unsigned serial = 1;

        while (std::filesystem::exists(backup))
            backup = destination.string() + ".previous-" + std::to_string(serial++);

        std::filesystem::rename(destination, backup);
    }

    std::filesystem::rename(temporary, destination);
}
} // namespace

// The native layout and record checks below hold for any texture that
// passed ValidateSource and the inventory's checks.
Status ConvertTexture(const TextureInventory::Texture& s, std::span<const uint8_t> data, uint64_t fileOffset, const std::string& path, TextureConversion& out) {
    MH3_RETURN_IF_ERROR(TextureInventory::ValidateSource(data, fileOffset, s));
    const aurora::ArrayRef<uint8_t> bytes{data.data() + s.offset, s.bytes};
    auto converted = s.entries
                         ? aurora::gfx::convert_texture_palette(s.format, s.width, s.height, s.mips, bytes, GXTlutFmt(s.paletteFormat), s.entries, {data.data() + s.palette, size_t(s.entries) * 2})
                         : aurora::gfx::convert_texture(s.format, s.width, s.height, s.mips, bytes);
    if (converted.data.empty())
        return Status::Error("Texture conversion failed: " + path);

    out = {};
    auto& r = out.record;
    r.id = s.id;
    r.format = 1;
    r.width = s.width;
    r.height = s.height;
    r.mips = s.mips;
    r.flags = converted.hasArbitraryMips ? 1u : 0u;

    if (s.format == GX_TF_CMPR && std::has_single_bit(s.width) && std::has_single_bit(s.height)) {
        const bool alpha = has_transparency(s, bytes.data());
        r.format = alpha ? 3 : 2;
        out.payload = compressed(s, bytes.data(), alpha);
    } else
        out.payload = linear(s, converted.data.data());

    const auto layout = aurora::gfx::native_texture_layout(s.width, s.height, s.mips, aurora::gfx::native_texture_format(r.format));

    if (out.payload.size() != layout.total_bytes)
        return Status::Error("Texture payload disagrees with native storage: " + path);

    r.image_bytes = layout.image_bytes;
    r.palette_offset = layout.palette_offset;
    r.palette_bytes = layout.palette_bytes;
    r.source_format = s.format;
    r.source_palette_entries = s.entries;
    r.source_palette_format = s.entries ? s.paletteFormat : 0;
    const int64_t delta = int64_t(s.palette) - s.offset;

    if (s.entries && (delta < INT32_MIN || delta > INT32_MAX))
        return Status::Error("Texture palette offset exceeds native record: " + path);

    r.source_palette_delta = s.entries ? int32_t(delta) : 0;
    r.bytes = out.payload.size();
    return {};
}

Status TexturePackWriter::Open(const std::filesystem::path& path, uint32_t count, uint32_t firstId) {
    output.open(path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    records.assign(count, {});
    base = firstId - 1;

    if (!output)
        return Status::Error("Cannot create native texture pack " + path.string());

    const std::array<uint32_t, 8> header{0x5456334d, 2, count, sizeof(Record), base, 0, 0, 0};

    for (auto word : header)
        Little32(output, word);

    std::vector<uint8_t> zero(records.size() * sizeof(Record));
    output.write(reinterpret_cast<const char*>(zero.data()), zero.size());
    return {};
}

Status TexturePackWriter::Add(TextureConversion&& texture) {
    if (added == records.size())
        return Status::Error("Native texture pack overflow");

    auto& r = texture.record;
    r.offset = (uint64_t(output.tellp()) + 63) & ~uint64_t(63);
    const uint64_t previousEnd = added ? records[added - 1].offset + records[added - 1].bytes : 32 + uint64_t(records.size()) * sizeof(Record);
    aurora::gfx::validate_native_texture_record(r, 2, base + added + 1, previousEnd, r.offset + r.bytes);
    output.seekp(r.offset);
    output.write(reinterpret_cast<const char*>(texture.payload.data()), texture.payload.size());

    if (!output)
        return Status::Error("Cannot write native texture payload");

    records[added++] = r;
    return {};
}

Status TexturePackWriter::Finish() {
    if (added != records.size())
        return Status::Error("Native texture pack is incomplete");

    output.seekp(32);

    for (const auto& r : records) {
        for (uint32_t word : std::array<uint32_t, 16>{r.id, r.format, r.width, r.height, r.mips, r.flags, r.bytes, r.palette_offset, uint32_t(r.offset), uint32_t(r.offset >> 32), r.image_bytes,
                                                      r.palette_bytes, std::bit_cast<uint32_t>(r.source_palette_delta), r.source_palette_entries, r.source_palette_format, r.source_format})
            Little32(output, word);
    }

    output.close();

    if (!output)
        return Status::Error("Cannot close native texture pack");

    return {};
}

void PrepareTextures(const std::filesystem::path& root, const Progress& progress) {
    // Read authored resource headers, even when repairing an interrupted pack.
    DiscImage disc(root, false);
    const auto names = TextureInventory::SourceNames(disc.FileNames());
    std::vector<Source> sources;
    std::vector<uint8_t> data;
    size_t processed = 0;

    for (const auto& path : names) {
        progress("Indexing game textures", processed++, names.size());

        if (disc.FileSize(path) > TextureInventory::MaxSourceBytes)
            throw std::runtime_error("Oversized texture source: " + path);

        data = disc.ReadFile(path);
        std::vector<TextureInventory::Texture> textures;

        // RMHP08's largest BRRES is 5.4 MiB. Source and dimension limits bound
        // conversion buffers; dictionaries and names have separate parser caps.
        CheckStatus(TextureInventory::Parse(data, disc.FileOffset(path), textures, uint32_t(sources.size() + 1)));

        for (auto& texture : textures) {
            Source source;
            static_cast<TextureInventory::Texture&>(source) = std::move(texture);
            source.path = path;
            sources.push_back(std::move(source));
        }
    }

    data.clear();
    data.shrink_to_fit();
    TexturePackWriter output;
    CheckStatus(output.Open(root / "native-textures.bin.preparing", uint32_t(sources.size()), 1));
    std::string path;

    for (size_t i = 0; i < sources.size(); ++i) {
        const auto& s = sources[i];
        progress("Converting game textures", i, sources.size());

        if (s.path != path) {
            path = s.path;

            if (disc.FileSize(path) > TextureInventory::MaxSourceBytes)
                throw std::runtime_error("Oversized texture source reread");

            data = disc.ReadFile(path);
        }

        TextureConversion converted;
        CheckStatus(ConvertTexture(s, data, disc.FileOffset(path), path, converted));
        CheckStatus(output.Add(std::move(converted)));
    }

    CheckStatus(output.Finish());

    // Publish the patch table after all of its referenced payloads are complete.
    std::sort(sources.begin(), sources.end(), [](const auto& a, const auto& b) { return a.patch < b.patch; });
    std::ofstream map(root / "native-texture-map.bin.preparing", std::ios::binary | std::ios::trunc);
    Little32(map, 0x4d54334d);
    Little32(map, 1);
    Little32(map, sources.size());

    for (const auto& s : sources) {
        Little64(map, s.patch);
        const uint32_t tag = 0xa0000000 | s.id;
        const char b[] = {char(tag >> 24), char(tag >> 16), char(tag >> 8), char(tag)};
        map.write(b, 4);
    }

    map.close();

    if (!map)
        throw std::runtime_error("Cannot close native texture patch table");
#ifdef __vita__
    if (sceIoSync("ux0:", 0) < 0)
        throw std::runtime_error("Cannot sync converted textures");
#endif
    Publish(root / "native-textures.bin.preparing", root / "native-textures.bin");
    Publish(root / "native-texture-map.bin.preparing", root / "native-texture-map.bin");
    progress("Native textures ready", sources.size(), sources.size());
}
} // namespace VitaLauncher
