#include "gx_stream.h"
#include <stdexcept>
#include <array>
#include <string>
#include <algorithm>
#include <bit>
#include <cstring>
#ifdef MH3_HAS_AURORA
#include <xxhash.h>
#endif

namespace {
uint32_t TextureContentsVersion(const void* data, size_t size) {
#ifdef MH3_HAS_AURORA
    const uint64_t digest = XXH3_64bits(data, size);
#else
    uint64_t digest = 14695981039346656037ull;
    for (const uint8_t byte : std::span(static_cast<const uint8_t*>(data), size))
        digest = (digest ^ byte) * 1099511628211ull;
#endif
    return uint32_t(digest) ^ uint32_t(digest >> 32);
}
}

std::array<GuestGxStream::Attribute, 12> GuestGxStream::Attributes(unsigned format) const {
    const auto lo = cp[0x50], hi = cp[0x60];
    const auto a = cp[0x70 + format], b = cp[0x80 + format], c = cp[0x90 + format];
    std::array<Attribute, 12> result{};
    auto numeric = [](unsigned descriptor, unsigned type, unsigned components, unsigned indices = 1) -> Attribute {
        if (!descriptor) return {};
        constexpr unsigned bytes[]{1, 1, 2, 2, 4};
        if (type >= std::size(bytes)) throw std::runtime_error("Invalid GX vertex component type");
        return {descriptor, bytes[type] * components, indices};
    };
    result[0] = numeric((lo >> 9) & 3, (a >> 1) & 7, 2 + (a & 1));
    const bool nbt3 = (a >> 31) != 0;
    result[1] = numeric((lo >> 11) & 3, (a >> 10) & 7, ((a & 0x200) || nbt3) ? 9 : 3, nbt3 ? 3 : 1);
    for (unsigned i = 0; i < 2; ++i) {
        const unsigned descriptor = (lo >> (13 + 2 * i)) & 3;
        if (descriptor) {
            constexpr unsigned bytes[]{2, 3, 4, 2, 3, 4};
            const unsigned type = (a >> (14 + 4 * i)) & 7;
            if (type >= std::size(bytes)) throw std::runtime_error("Invalid GX vertex color type");
            result[2 + i] = {descriptor, bytes[type], 1};
        }
    }
    const uint32_t texFields[]{a >> 21, b, b >> 9, b >> 18, b >> 27, c >> 5, c >> 14, c >> 23};
    for (unsigned i = 0; i < 8; ++i)
        result[4 + i] = numeric((hi >> (2 * i)) & 3, (texFields[i] >> 1) & 7, 1 + (texFields[i] & 1));
    return result;
}
size_t GuestGxStream::VertexSize(unsigned format) const {
    size_t size = std::popcount(cp[0x50] & 0x1FF);
    for (const auto& attribute : Attributes(format))
        size += attribute.descriptor >= 2 ? (attribute.descriptor - 1) * attribute.indices : attribute.bytes;
    return size;
}
std::array<uint8_t, 16> GuestGxStream::ArrayPacket(unsigned slot, ArrayView view) {
    std::array<uint8_t, 16> packet{};
    packet[0] = 0x50; packet[2] = 0x10 + slot;
    for (unsigned i = 0; i < 8; ++i) packet[3 + i] = uint64_t(view.address) >> (56 - 8 * i);
    for (unsigned i = 0; i < 4; ++i) packet[11 + i] = view.size >> (24 - 8 * i);
    return packet;
}
void GuestGxStream::BindDrawArrays(std::span<const uint8_t> draw, const Sink& sink, const ArrayResolver& resolver) {
    const unsigned count = (unsigned(draw[1]) << 8) | draw[2];
    if (!count) return;
    const auto attributes = Attributes(draw[0] & 7);
    if (std::none_of(attributes.begin(), attributes.end(), [](auto a) { return a.descriptor >= 2; })) return;
    if (!resolver) throw std::runtime_error("Indexed draw requires guest vertex-array resolution");
    std::array<uint32_t, 12> maximum{};
    size_t offset = 3;
    const unsigned matrixBytes = std::popcount(cp[0x50] & 0x1FF);
    for (unsigned vertex = 0; vertex < count; ++vertex) {
        offset += matrixBytes;
        for (unsigned slot = 0; slot < attributes.size(); ++slot) {
            const auto& a = attributes[slot];
            if (a.descriptor < 2) { offset += a.bytes; continue; }
            for (unsigned i = 0; i < a.indices; ++i) {
                if (offset + a.descriptor - 1 > draw.size()) throw std::runtime_error("GX index exceeds vertex packet");
                const unsigned index = a.descriptor == 2 ? draw[offset] : (unsigned(draw[offset]) << 8) | draw[offset + 1];
                maximum[slot] = std::max(maximum[slot], index);
                offset += a.descriptor - 1;
            }
        }
    }
    if (offset != draw.size()) throw std::runtime_error("GX array scan disagrees with vertex framing");
    for (unsigned slot = 0; slot < attributes.size(); ++slot) {
        const auto& a = attributes[slot];
        if (a.descriptor < 2) continue;
        auto view = resolver(cp[0xA0 + slot]);
        // NBT3 carries three independent indices into three-component vectors;
        // ordinary NBT carries one index into a nine-component array element.
        const uint64_t needed = uint64_t(maximum[slot]) * (cp[0xB0 + slot] & 255) + a.bytes / a.indices;
        if (!view.address || needed > view.size)
            throw std::runtime_error("GX draw index exceeds guest vertex-array RAM");
        view.size = static_cast<uint32_t>(needed);
        auto& bound = boundArrays[slot];
        if (bound.address != view.address || bound.size != view.size) {
            sink(ArrayPacket(slot, view));
            bound = view;
        }
    }
}

std::array<uint8_t, 37> GuestGxStream::TexturePacket(unsigned slot, const ArrayResolver& resolver) {
    if (!resolver) throw std::runtime_error("Guest GX texture requires host pointer conversion");
    const unsigned bank = slot < 4 ? 0x80 : 0xA0;
    const unsigned index = slot & 3;
    const auto image = bp[bank + 8 + index];
    const uint32_t width = (image & 1023) + 1, height = ((image >> 10) & 1023) + 1;
    const uint32_t format = (image >> 20) & 15;
    unsigned tileW = 4, tileH = 4, tileBytes = 32;
    switch (format) {
    case 0: case 8: case 14: tileW = tileH = 8; break; // I4, C4, CMPR
    case 1: case 2: case 9: tileW = 8; break; // I8, IA4, C8
    case 3: case 4: case 5: case 10: break; // IA8, RGB565, RGB5A3, C14X2
    case 6: tileBytes = 64; break; // RGBA8
    default: throw std::runtime_error("Guest GX texture format requires palette/format support: " + std::to_string(format));
    }
    const bool mips = ((bp[bank + index] >> 5) & 3) != 0;
    uint32_t needed = 0, w = width, h = height;
    do {
        needed += ((w + tileW - 1) / tileW) * ((h + tileH - 1) / tileH) * tileBytes;
        if (!mips || (w == 1 && h == 1)) break;
        w = std::max(1u, w / 2); h = std::max(1u, h / 2);
    } while (true);
    const uint32_t guest = bp[bank + 20 + index] << 5;
    const auto view = resolver(guest);
    if (view.size < needed) throw std::runtime_error("Guest GX texture exceeds its RAM region");
    std::array<uint8_t, 37> out{};
    out[0] = 0x50; out[2] = 0x30; out[3] = slot;
    for (unsigned i = 0; i < 8; ++i) out[4 + i] = uint64_t(view.address) >> (56 - 8 * i);
    auto put = [&](unsigned offset, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) out[offset + i] = value >> (24 - 8 * i);
    };
    put(12, width); put(16, height); put(20, format);
    // Each texture unit gets its own host TLUT slot. Guest TMEM offsets and
    // formats are resolved immediately before the draw that uses the palette.
    if (format >= 8 && format <= 10) put(24, slot);
    // Aurora keys its decoded-texture cache on this revision. A fresh number
    // for every BP write defeats that cache; hash the actual guest bytes so a
    // rewrite at the same address still invalidates the decoded texture.
    out[28] = mips;
    put(29, guest); put(33, TextureContentsVersion(reinterpret_cast<const void*>(view.address), needed));
    return out;
}

void GuestGxStream::LoadPalette(const ArrayResolver& resolver) {
    paletteBindings = {};
    const uint32_t size = ((bp[0x65] >> 10) & 0x7FF) * 32;
    if (!size) return;
    if (!resolver) throw std::runtime_error("Guest GX TLUT load requires RAM resolution");
    const auto view = resolver(bp[0x64] << 5);
    if (!view.address || size > view.size) throw std::runtime_error("Guest GX TLUT load exceeds RAM");
    if (!palettes) palettes = std::make_shared<PaletteMemory>();
    const uint32_t destination = (bp[0x65] & 1023) * 512;
    // Preserve the DMA snapshot even if the guest rewrites or frees its source.
    std::memcpy(palettes->bytes.data() + destination, reinterpret_cast<const void*>(view.address), size);
    std::fill_n(palettes->loadedLines.begin() + destination / 32, size / 32, true);
    if (++palettes->revision == 0) throw std::runtime_error("Guest GX TLUT revision overflow");
}

void GuestGxStream::BindDrawPalettes(const Sink& sink, const ArrayResolver& resolver) {
    unsigned used = 0;
    for (unsigned stage = 0; stage <= ((bp[0] >> 10) & 15); ++stage) {
        const auto order = bp[0x28 + stage / 2] >> ((stage & 1) * 12);
        if (order & 64) used |= 1u << (order & 7);
    }
    const unsigned indirect = (bp[0] >> 16) & 7;
    if (indirect > 4) throw std::runtime_error("Invalid GX indirect stage count");
    for (unsigned stage = 0; stage < indirect; ++stage) used |= 1u << ((bp[0x27] >> (stage * 6)) & 7);
    for (unsigned slot = 0; slot < 8; ++slot) {
        if (!(used & (1u << slot)) || !textureBound[slot]) continue;
        const unsigned bank = slot < 4 ? 0x80 : 0xA0, index = slot & 3;
        const unsigned format = (bp[bank + 8 + index] >> 20) & 15;
        if (format < 8 || format > 10) continue;
        const uint32_t descriptor = bp[bank + 24 + index] & 0xFFF;
        const unsigned paletteFormat = descriptor >> 10;
        if (paletteFormat > 2) throw std::runtime_error("Invalid GX TLUT format");
        const unsigned entries = format == 8 ? 16 : format == 9 ? 256 : 16384;
        const unsigned start = (descriptor & 1023) * 512;
        if (!palettes || !std::all_of(palettes->loadedLines.begin() + start / 32,
            palettes->loadedLines.begin() + (start + entries * 2) / 32, [](bool loaded) { return loaded; }))
            throw std::runtime_error("Guest GX texture uses an incompletely loaded palette");
        auto& bound = paletteBindings[slot];
        if (bound.revision == palettes->revision && bound.descriptor == descriptor && bound.entries == entries) continue;
        std::array<uint8_t, 26> packet{};
        packet[0] = 0x50; packet[2] = 0x31; packet[3] = slot;
        const auto address = reinterpret_cast<uintptr_t>(palettes->bytes.data() + start);
        for (unsigned i = 0; i < 8; ++i) packet[4 + i] = uint64_t(address) >> (56 - 8 * i);
        packet[15] = paletteFormat;
        packet[16] = entries >> 8; packet[17] = entries;
        // Stable identity per host slot; revision also changes for overlapping
        // TMEM writes and descriptor/format changes at the same destination.
        const uint32_t identity = 0x80000000u | slot;
        const uint32_t revision = ++paletteRevision;
        for (unsigned i = 0; i < 4; ++i) {
            packet[18 + i] = identity >> (24 - 8 * i);
            packet[22 + i] = revision >> (24 - 8 * i);
        }
        sink(packet);
        // Refresh the texture binding after the TLUT revision changes so the
        // next draw resolves the new palette with the same source bytes.
        sink(TexturePacket(slot, resolver));
        bound = {palettes->revision, descriptor, entries};
    }
}

void GuestGxStream::Write(std::span<const uint8_t> bytes, const Sink& sink, const ArrayResolver& resolver,
                          const CopySink& copy, const TextureCopySink& textureCopy) {
    // A 65535-vertex direct packet can exceed 8 MiB with all attributes.
    constexpr size_t limit = 16 * 1024 * 1024;
    if (bytes.size() > limit || pending.size() + bytes.size() > limit)
        throw std::runtime_error("Guest GX command buffer exceeds 16 MiB");
    pending.insert(pending.end(), bytes.begin(), bytes.end());
    size_t offset = 0;
    while (offset < pending.size()) {
        const auto* p = pending.data() + offset;
        const size_t available = pending.size() - offset;
        size_t length;
        switch (p[0]) {
        case 0x00: case 0x48: length = 1; break;
        case 0x08: length = 6; break;
        case 0x61: length = 5; break;
        case 0x40: length = 9; break;
        case 0x20: case 0x28: case 0x30: case 0x38: length = 5; break;
        case 0x10:
            if (available < 5) goto incomplete;
            length = 5 + 4 * (1 + ((size_t(p[1]) << 8) | p[2]));
            break;
        default:
            if (p[0] >= 0x80 && p[0] <= 0xBF) {
                if (available < 3) goto incomplete;
                const auto vertices = (size_t(p[1]) << 8) | p[2];
                length = 3 + vertices * VertexSize(p[0] & 7);
                break;
            }
            throw std::runtime_error("Guest GX opcode requires a bridge: " + std::to_string(p[0]));
        }
        if (available < length) break;
        if (p[0] == 0x20 || p[0] == 0x28 || p[0] == 0x30 || p[0] == 0x38) {
            if (!resolver) throw std::runtime_error("Indexed XF load requires guest RAM resolution");
            const unsigned array = 12 + (p[0] - 0x20) / 8;
            const uint32_t index = (uint32_t(p[1]) << 8) | p[2];
            const unsigned words = (p[3] >> 4) + 1;
            const unsigned destination = ((p[3] & 15) << 8) | p[4];
            // Match Aurora's supported XF memory writes explicitly. Its decoder
            // logs/skips unsupported forms, which must not look successful here.
            const bool supported =
                (destination < 0x78 && destination % 12 == 0 && words == 12) ||
                (destination >= 0x78 && destination < 0xF0 && (destination - 0x78) % 12 == 0 && (words == 8 || words == 12)) ||
                (destination >= 0x400 && destination < 0x45A && (destination - 0x400) % 9 == 0 && words == 9) ||
                (destination >= 0x500 && destination < 0x5F0 && (destination - 0x500) % 12 == 0 && words == 12) ||
                (destination >= 0x600 && destination < 0x680 && (destination & 15) + words <= 16);
            if (!supported) throw std::runtime_error("Unsupported indexed XF destination or partial matrix write");
            const uint32_t stride = cp[0xB0 + array] & 255;
            const uint64_t sourceOffset = uint64_t(index) * stride;
            const auto view = resolver(cp[0xA0 + array]);
            if (!stride || !view.address || sourceOffset + words * 4 > view.size)
                throw std::runtime_error("Indexed XF source exceeds guest matrix array");
            std::array<uint8_t, 69> direct{};
            direct[0] = 0x10; direct[2] = words - 1;
            direct[3] = destination >> 8; direct[4] = destination;
            const auto* source = reinterpret_cast<const uint8_t*>(view.address) + sourceOffset;
            std::copy_n(source, words * 4, direct.begin() + 5);
            sink(std::span<const uint8_t>(direct.data(), 5 + words * 4));
            offset += length;
            continue;
        }
        if (p[0] == 0x40) {
            const uint32_t address = (uint32_t(p[1]) << 24) | (uint32_t(p[2]) << 16) |
                                     (uint32_t(p[3]) << 8) | p[4];
            const uint32_t size = (uint32_t(p[5]) << 24) | (uint32_t(p[6]) << 16) |
                                  (uint32_t(p[7]) << 8) | p[8];
            if (size) {
                if (!resolver) throw std::runtime_error("GX display list requires guest RAM resolution");
                if (size > limit || displayListDepth >= 16)
                    throw std::runtime_error("GX display list exceeds size or nesting limit");
                const auto view = resolver(address);
                if (!view.address || size > view.size)
                    throw std::runtime_error("GX display list exceeds guest RAM");
                // A separate framer avoids invalidating the parent's partial FIFO
                // packet while processing nested lists. GPU state flows through
                // the call exactly as if its commands appeared inline.
                GuestGxStream child;
                child.bp = bp; child.cp = cp; child.bpMask = bpMask;
                child.textureBound = textureBound; child.paletteRevision = paletteRevision;
                child.boundArrays = boundArrays;
                child.palettes = palettes; child.paletteBindings = paletteBindings;
                child.displayListDepth = displayListDepth + 1;
                child.Write({reinterpret_cast<const uint8_t*>(view.address), size}, sink, resolver, copy, textureCopy);
                if (child.PendingBytes()) throw std::runtime_error("GX display list ends in a partial command");
                bp = child.bp; cp = child.cp; bpMask = child.bpMask;
                textureBound = child.textureBound; paletteRevision = child.paletteRevision;
                boundArrays = child.boundArrays;
                palettes = child.palettes; paletteBindings = child.paletteBindings;
            }
            offset += length;
            continue;
        }
        if (p[0] == 0x08)
            cp[p[1]] = (uint32_t(p[2]) << 24) | (uint32_t(p[3]) << 16) | (uint32_t(p[4]) << 8) | p[5];
        if (p[0] == 0x08 && p[1] >= 0xA0 && p[1] <= 0xAF) {
            if (!resolver) throw std::runtime_error("Guest GX vertex-array address requires host pointer conversion");
            const uint32_t guest = (uint32_t(p[2]) << 24) | (uint32_t(p[3]) << 16) | (uint32_t(p[4]) << 8) | p[5];
            const auto view = resolver(guest);
            // Aurora's 0x50/0x0010..1F extension carries a host pointer,
            // accessible byte count, and an explicit guest-big-endian flag.
            sink(ArrayPacket(p[1] - 0xA0, view));
            boundArrays[p[1] - 0xA0] = view;
            offset += length;
            continue;
        }
        if (p[0] == 0x61) {
            const auto reg = p[1];
            const uint32_t value = (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 8) | p[4];
            if (reg == 0xFE) bpMask = value;
            else {
                bp[reg] = (bp[reg] & ~bpMask) | (value & bpMask);
                bpMask = 0xFFFFFF;
            }
            if (reg == 0x65) LoadPalette(resolver);
            const unsigned bank = reg >= 0xA0 ? 0xA0 : 0x80;
            const unsigned group = (reg - bank) & ~3u;
            if (reg >= bank && reg < bank + 28 && (group == 0 || group == 4 || group == 8 || group == 20 || group == 24)) {
                const unsigned slot = (reg & 3) + (bank == 0xA0 ? 4 : 0);
                if (group == 20) textureBound[slot] = true;
                if (textureBound[slot]) {
                    const auto metadata = TexturePacket(slot, resolver);
                    sink(std::span<const uint8_t>(p, length));
                    sink(metadata);
                    offset += length;
                    continue;
                }
            }
            if (reg == 0x52) {
                const uint32_t trigger = bp[reg];
                if (!(trigger & 0x4000)) {
                    if (!textureCopy || !resolver) throw std::runtime_error("GX texture copy requires a RAM/cache bridge");
                    const uint32_t encodedFormat = (trigger >> 3) & 15;
                    const uint32_t format = (encodedFormat >> 1) | ((encodedFormat & 1) << 3);
                    if ((bp[0x43] & 7) == 3 || !(trigger & 0x10000) || format > 6 ||
                        ((format <= 3) != bool(trigger & 0x8000)))
                        throw std::runtime_error("GX texture copy requires depth/channel format conversion");
                    const uint32_t sourceWidth = (bp[0x4A] & 1023) + 1;
                    const uint32_t sourceHeight = ((bp[0x4A] >> 10) & 1023) + 1;
                    const bool halfScale = (trigger & 0x200) != 0;
                    const uint32_t width = std::max(1u, sourceWidth >> unsigned(halfScale));
                    const uint32_t height = std::max(1u, sourceHeight >> unsigned(halfScale));
                    const uint32_t tileWidth = format <= 2 ? 8 : 4;
                    const uint32_t tileHeight = format == 0 ? 8 : 4;
                    const uint32_t tileBytes = format == 6 ? 64 : 32;
                    const uint32_t rowBytes = ((width + tileWidth - 1) / tileWidth) * tileBytes;
                    if ((bp[0x4D] & 1023) * 32 != rowBytes)
                        throw std::runtime_error("GX texture copy requires strided RAM output");
                    const auto view = resolver(bp[0x4B] << 5);
                    if (uint64_t(rowBytes) * ((height + tileHeight - 1) / tileHeight) > view.size)
                        throw std::runtime_error("GX texture copy exceeds guest RAM");
                    sink(std::span<const uint8_t>(p, length));
                    textureCopy({view.address, bp[0x49] & 1023, (bp[0x49] >> 10) & 1023,
                                 sourceWidth, sourceHeight, width, height, format, halfScale, (trigger & 0x800) != 0});
                    offset += length;
                    continue;
                }
                if (!copy || !resolver) throw std::runtime_error("GX display copy requires a host surface bridge");
                const uint32_t width = (bp[0x4D] & 1023) * 16;
                const uint32_t sourceHeight = ((bp[0x4A] >> 10) & 1023) + 1;
                const uint32_t scale = bp[0x4E] & 511;
                // BP bit 10 selects reciprocal scaling, not a scaling enable.
                // Match the hardware copy interval count (height minus one).
                if ((trigger & 0x400) && scale == 0)
                    throw std::runtime_error("GX reciprocal display-copy scale is zero");
                const uint32_t destinationHeight = 1 + ((trigger & 0x400)
                    ? (sourceHeight - 1) * 256 / scale
                    : (sourceHeight - 1) * scale / 256);
                if (width == 0 || width > 1024) throw std::runtime_error("GX display copy has invalid stride");
                const auto view = resolver(bp[0x4B] << 5);
                if (uint64_t(width) * destinationHeight * 2 > view.size)
                    throw std::runtime_error("GX display copy exceeds guest framebuffer RAM");
                sink(std::span<const uint8_t>(p, length));
                copy({view.address, width, destinationHeight, (trigger & 0x800) != 0});
                offset += length;
                continue;
            }
            if (reg == 0x45 || reg == 0x47 || reg == 0x48)
                throw std::runtime_error("Guest GX copy/token/finish requires event integration (BP register " + std::to_string(reg) + ")");
        }
        if (p[0] >= 0x80 && p[0] <= 0xBF) {
            BindDrawArrays(std::span<const uint8_t>(p, length), sink, resolver);
            BindDrawPalettes(sink, resolver);
        }
        sink(std::span<const uint8_t>(p, length));
        offset += length;
    }
incomplete:
    pending.erase(pending.begin(), pending.begin() + offset);
}
