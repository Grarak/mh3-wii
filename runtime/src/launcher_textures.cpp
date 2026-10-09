// Startup conversion of external disc textures. The swizzle/CMPR routines
// mirror the verified host packer in scripts/vita_texture_pack.cpp.
#include "vita_launcher.h"
#include "fatal.h"
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
        // Morton order over the blocks of the size rounded up to powers of two.
        const uint32_t bw = std::max(std::bit_ceil(w) / 4, 1u), bh = std::max(std::bit_ceil(h) / 4, 1u);
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

// Up to 256 distinct RGBA words in first-use order: the palette of a P4/P8
// texture. A full palette has seen a 257th colour.
struct Palette {
    std::array<uint32_t, 512> keys{};
    std::array<uint16_t, 512> slots{}; // index + 1, 0 empty
    std::array<uint32_t, 256> colors{};
    uint32_t count = 0;
    bool full = false;

    static unsigned Hash(uint32_t color) {
        return (color * 0x9e3779b1u) >> 23;
    }

    int Find(uint32_t color) const {
        for (unsigned at = Hash(color);; at = (at + 1) & 511) {
            if (!slots[at])
                return -1;

            if (keys[at] == color)
                return slots[at] - 1;
        }
    }

    void Add(uint32_t color) {
        unsigned at = Hash(color);

        for (; slots[at]; at = (at + 1) & 511)
            if (keys[at] == color)
                return;

        if (count == 256) {
            full = true;
            return;
        }

        keys[at] = color;
        slots[at] = uint16_t(count + 1);
        colors[count++] = color;
    }
};

// GXM swizzles any size, but other sizes than powers of two only with one level.
bool Swizzles(const TextureInventory::Texture& s) {
    return s.mips == 1 || (std::has_single_bit(s.width) && std::has_single_bit(s.height));
}

uint32_t Word(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

size_t TexelCount(const TextureInventory::Texture& s) {
    size_t texels = 0;

    for (unsigned mip = 0; mip < s.mips; ++mip)
        texels += size_t(std::max(s.width >> mip, 1u)) * std::max(s.height >> mip, 1u);

    return texels;
}

bool Exact565(const uint8_t* p) {
    return p[3] == 255 && ((p[0] & 0xf8) | p[0] >> 5) == p[0] && ((p[1] & 0xfc) | p[1] >> 6) == p[1] && ((p[2] & 0xf8) | p[2] >> 5) == p[2];
}

// The smallest native format that samples every texel of the decoded RGBA
// unchanged (tags of native_texture_format): a P4/P8 palette for up to 16/256
// colours (swizzled: one level for other sizes than powers of two), U8 for grey or alpha-only texels, U8U8
// for grey with alpha, RGB565 for opaque 5:6:5 colours, else RGBA8. Equal
// sizes prefer the earlier, palette-free format.
uint32_t ExactFormat(const TextureInventory::Texture& s, const uint8_t* rgba, Palette& palette) {
    bool gray = true, alphaGray = true, opaque = true, black = true, white = true, rgb565 = true;
    const bool swizzle = Swizzles(s);
    const size_t texels = TexelCount(s);

    for (size_t i = 0; i < texels; ++i, rgba += 4) {
        const bool g = rgba[0] == rgba[1] && rgba[0] == rgba[2];
        gray &= g;
        alphaGray &= g && rgba[3] == rgba[0];
        opaque &= rgba[3] == 255;
        black &= g && rgba[0] == 0;
        white &= g && rgba[0] == 255;
        rgb565 &= Exact565(rgba);

        if (swizzle && !palette.full)
            palette.Add(Word(rgba));
    }

    uint32_t best = 1;
    size_t bestBytes = aurora::gfx::native_texture_layout(s.width, s.height, s.mips, aurora::gfx::native_texture_format(1)).total_bytes;
    const auto consider = [&](bool usable, uint32_t format) {
        if (!usable)
            return;

        const size_t bytes = aurora::gfx::native_texture_layout(s.width, s.height, s.mips, aurora::gfx::native_texture_format(format)).total_bytes;

        if (bytes < bestBytes) {
            best = format;
            bestBytes = bytes;
        }
    };

    consider(alphaGray, 6);
    consider(gray && opaque, 7);
    consider(black, 8);
    consider(white, 9);
    consider(gray, 10);
    consider(rgb565, 11);
    consider(swizzle && palette.count <= 16, 4);
    consider(swizzle && !palette.full, 5);
    return best;
}

// The decoded RGBA levels in a linear or palette native format.
std::vector<uint8_t> native(const TextureInventory::Texture& s, const uint8_t* rgba, uint32_t tag, const Palette& palette) {
    const auto format = aurora::gfx::native_texture_format(tag);
    const auto layout = aurora::gfx::native_texture_layout(s.width, s.height, s.mips, format);
    std::vector<uint8_t> output(layout.total_bytes);
    const bool p4 = tag == 4, p8 = tag == 5;

    if (p4 || p8)
        for (uint32_t i = 0; i < palette.count; ++i)
            std::memcpy(output.data() + layout.palette_offset + i * 4, &palette.colors[i], 4);

    for (const auto& level : layout.levels) {
        auto* dest = output.data() + level.offset;

        for (uint32_t y = 0; y < level.height; ++y)
            for (uint32_t x = 0; x < level.width; ++x, rgba += 4) {
                if (p4 || p8) {
                    const auto index = uint8_t(palette.Find(Word(rgba)));
                    const auto at = morton(x, y, std::bit_ceil(level.width), std::bit_ceil(level.height));

                    if (p4)
                        dest[at / 2] |= uint8_t(index << (4 * (at & 1)));
                    else
                        dest[at] = index;

                    continue;
                }

                auto* out = dest + size_t(y) * level.pitch;

                switch (tag) {
                case 1:
                    std::memcpy(out + x * 4, rgba, 4);
                    break;
                case 6:
                case 7:
                case 10:
                    out[x * (tag == 10 ? 2 : 1)] = rgba[0];

                    if (tag == 10)
                        out[x * 2 + 1] = rgba[3];

                    break;
                case 8:
                case 9:
                    out[x] = rgba[3];
                    break;
                case 11: {
                    const uint16_t v = uint16_t((rgba[0] >> 3) << 11 | (rgba[1] >> 2) << 5 | rgba[2] >> 3);
                    out[x * 2] = uint8_t(v);
                    out[x * 2 + 1] = uint8_t(v >> 8);
                    break;
                }
                }
            }
    }

    return output;
}

aurora::gfx::ConvertedTexture Decode(const TextureInventory::Texture& s, std::span<const uint8_t> data) {
    const aurora::ArrayRef<uint8_t> bytes{data.data() + s.offset, s.bytes};
    return s.entries ? aurora::gfx::convert_texture_palette(s.format, s.width, s.height, s.mips, bytes, GXTlutFmt(s.paletteFormat), s.entries, {data.data() + s.palette, size_t(s.entries) * 2})
                     : aurora::gfx::convert_texture(s.format, s.width, s.height, s.mips, bytes);
}

bool Compressed(const TextureInventory::Texture& s) {
    return s.format == GX_TF_CMPR && Swizzles(s);
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
    const auto converted = Decode(s, data);

    if (converted.data.empty())
        return Status::Error("Texture conversion failed: " + path);

    out = {};
    auto& r = out.record;
    r.id = s.id;
    r.width = s.width;
    r.height = s.height;
    r.mips = s.mips;
    r.flags = converted.hasArbitraryMips ? 1u : 0u;

    if (Compressed(s)) {
        const uint8_t* bytes = data.data() + s.offset;
        const bool alpha = has_transparency(s, bytes);
        r.format = alpha ? 3 : 2;
        out.payload = compressed(s, bytes, alpha);
    } else {
        Palette palette;
        r.format = ExactFormat(s, converted.data.data(), palette);
        out.payload = native(s, converted.data.data(), r.format, palette);
    }

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

Status TexturePayloadBytes(const TextureInventory::Texture& s, std::span<const uint8_t> data, uint64_t fileOffset, uint64_t& bytes) {
    MH3_RETURN_IF_ERROR(TextureInventory::ValidateSource(data, fileOffset, s));

    // ConvertTexture's choice: the same source decodes to the same texels.
    uint32_t format;

    if (Compressed(s))
        format = has_transparency(s, data.data() + s.offset) ? 3 : 2;
    else {
        const auto converted = Decode(s, data);

        if (converted.data.empty())
            return Status::Error("Texture conversion failed");

        Palette palette;
        format = ExactFormat(s, converted.data.data(), palette);
    }

    bytes = aurora::gfx::native_texture_layout(s.width, s.height, s.mips, aurora::gfx::native_texture_format(format)).total_bytes;
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

void TexturePackWriter::Plan(const std::vector<uint64_t>& bytes) {
    sizes = bytes;
    offsets.resize(bytes.size());
    uint64_t end = 32 + uint64_t(records.size()) * sizeof(Record);

    for (size_t i = 0; i < bytes.size(); ++i) {
        offsets[i] = (end + 63) & ~uint64_t(63);
        end = offsets[i] + bytes[i];
    }
}

Status TexturePackWriter::Put(uint32_t index, TextureConversion&& texture) {
    if (index >= offsets.size() || offsets.size() != records.size() || texture.payload.size() != sizes[index])
        return Status::Error("Native texture payload differs from its planned place");

    auto& r = texture.record;
    r.offset = offsets[index];
    const uint64_t previousEnd = index ? offsets[index - 1] + sizes[index - 1] : 32 + uint64_t(records.size()) * sizeof(Record);
    aurora::gfx::validate_native_texture_record(r, 2, base + index + 1, previousEnd, r.offset + r.bytes);
    std::lock_guard guard(lock);
    output.seekp(r.offset);
    output.write(reinterpret_cast<const char*>(texture.payload.data()), texture.payload.size());

    if (!output)
        return Status::Error("Cannot write native texture payload");

    records[index] = r;
    ++added;
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
            MH3_THROW(std::runtime_error("Oversized texture source: " + path));

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
                MH3_THROW(std::runtime_error("Oversized texture source reread"));

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
        MH3_THROW(std::runtime_error("Cannot close native texture patch table"));
#ifdef __vita__
    if (sceIoSync("ux0:", 0) < 0)
        MH3_THROW(std::runtime_error("Cannot sync converted textures"));
#endif
    Publish(root / "native-textures.bin.preparing", root / "native-textures.bin");
    Publish(root / "native-texture-map.bin.preparing", root / "native-texture-map.bin");
    progress("Native textures ready", sources.size(), sources.size());
}
} // namespace VitaLauncher
