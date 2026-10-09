#include "gx_stream.h"
#ifdef MH3_ENABLE_EFB_COPY_TRACE
#include "efb_copy_trace.h"
#endif
#include <stdexcept>
#include <array>
#include <string>
#include <algorithm>
#include <bit>
#include <cstring>
#include <unordered_map>
#include "aurora-main/include/aurora/native_assets.h"
#include "aurora-main/include/aurora/generated_texture.hpp"
#ifdef __vita__
#include "memory.h"
#endif
#ifdef MH3_HAS_AURORA
#include <xxhash.h>
#endif

namespace {
uint64_t ContentsDigest(const void* data, size_t size) {
#ifdef MH3_HAS_AURORA
    const uint64_t digest = XXH3_64bits(data, size);
#else
    uint64_t digest = 14695981039346656037ull;
    for (const uint8_t byte : std::span(static_cast<const uint8_t*>(data), size))
        digest = (digest ^ byte) * 1099511628211ull;
#endif
    return digest;
}
uint32_t TextureContentsVersion(const void* data, size_t size) {
    const uint64_t digest = ContentsDigest(data, size);
    return uint32_t(digest) ^ uint32_t(digest >> 32);
}

// Hashes of the textures loaded since the last invalidation, by host address.
// GX reads texture memory through TMEM, which keeps what it loaded until the
// game invalidates it (GXInvalidateTexAll/Region, BP 0x66): a texture drawn
// again before that needs no new hash. An EFB texture copy drops the textures
// it overwrites and each frame's display copy clears it, so a CPU change shows
// at the latest one frame on.
std::unordered_map<uintptr_t, std::pair<uint32_t, uint32_t>>& TextureHashes() {
    static std::unordered_map<uintptr_t, std::pair<uint32_t, uint32_t>> hashes;
    return hashes;
}
uint32_t CachedTextureVersion(uintptr_t address, uint32_t size) {
    auto& entry = TextureHashes()[address];
    if (entry.first != size) {
        entry = {size, TextureContentsVersion(reinterpret_cast<const void*>(address), size)};
        ++GuestGxStream::textureHashCount;
    }
    return entry.second;
}

// The index scan for the usual layouts: up to four fields of one width, with
// the maxima in registers. False for any other layout.
template <unsigned Width, unsigned N>
void ScanIndexFieldsN(const uint8_t* vertex, unsigned count, uint32_t stride, std::span<const GuestGxStream::IndexField> fields, std::array<uint32_t, 12>& maximum) {
    uint32_t offset[N], most[N]{};
    for (unsigned i = 0; i < N; ++i)
        offset[i] = fields[i].offset;
    for (unsigned v = 0; v < count; ++v, vertex += stride)
        for (unsigned i = 0; i < N; ++i) {
            const uint8_t* p = vertex + offset[i];
            most[i] = std::max(most[i], Width == 1 ? uint32_t(p[0]) : (uint32_t(p[0]) << 8) | p[1]);
        }
    for (unsigned i = 0; i < N; ++i)
        maximum[fields[i].slot] = std::max(maximum[fields[i].slot], most[i]);
}
template <unsigned Width>
bool ScanIndexFields(const uint8_t* vertex, unsigned count, uint32_t stride, std::span<const GuestGxStream::IndexField> fields, std::array<uint32_t, 12>& maximum) {
    switch (fields.size()) {
    case 1:
        ScanIndexFieldsN<Width, 1>(vertex, count, stride, fields, maximum);
        return true;
    case 2:
        ScanIndexFieldsN<Width, 2>(vertex, count, stride, fields, maximum);
        return true;
    case 3:
        ScanIndexFieldsN<Width, 3>(vertex, count, stride, fields, maximum);
        return true;
    case 4:
        ScanIndexFieldsN<Width, 4>(vertex, count, stride, fields, maximum);
        return true;
    default:
        return false;
    }
}
} // namespace

void GuestGxStream::InvalidateTextureHashes() {
    if (TextureHashes().empty())
        return;
    TextureHashes().clear();
    ++textureInvalidateCount;
}
void GuestGxStream::InvalidateTextureHashes(uintptr_t address, uint64_t size) {
    std::erase_if(TextureHashes(), [&](const auto& entry) { return entry.first < address + size && address < entry.first + entry.second.first; });
}

std::array<GuestGxStream::Attribute, 12> GuestGxStream::Attributes(unsigned format) const {
    const auto lo = cp[0x50], hi = cp[0x60];
    const auto a = cp[0x70 + format], b = cp[0x80 + format], c = cp[0x90 + format];
    std::array<Attribute, 12> result{};
    auto numeric = [](unsigned descriptor, unsigned type, unsigned components, unsigned indices = 1) -> Attribute {
        if (!descriptor)
            return {};
        constexpr unsigned bytes[]{1, 1, 2, 2, 4};
        if (type >= std::size(bytes))
            throw std::runtime_error("Invalid GX vertex component type");
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
            if (type >= std::size(bytes))
                throw std::runtime_error("Invalid GX vertex color type");
            result[2 + i] = {descriptor, bytes[type], 1};
        }
    }
    const uint32_t texFields[]{a >> 21, b, b >> 9, b >> 18, b >> 27, c >> 5, c >> 14, c >> 23};
    for (unsigned i = 0; i < 8; ++i)
        result[4 + i] = numeric((hi >> (2 * i)) & 3, (texFields[i] >> 1) & 7, 1 + (texFields[i] & 1));
    return result;
}
const GuestGxStream::VertexLayout& GuestGxStream::Layout(unsigned format) {
    auto& layout = layouts[format];
    const std::array<uint32_t, 5> key{cp[0x50], cp[0x60], cp[0x70 + format], cp[0x80 + format], cp[0x90 + format]};
    if (layout.valid && layout.key == key)
        return layout;
    layout = {};
    const auto attributes = Attributes(format);
    uint32_t offset = std::popcount(cp[0x50] & 0x1FF);
    for (unsigned slot = 0; slot < attributes.size(); ++slot) {
        const auto& a = attributes[slot];
        if (a.descriptor < 2) {
            offset += a.bytes;
            continue;
        }
        layout.indexedSlots |= 1u << slot;
        layout.bytes[slot] = a.bytes / a.indices;
        for (unsigned i = 0; i < a.indices; ++i) {
            layout.fields[layout.fieldCount++] = {uint16_t(offset), uint8_t(a.descriptor - 1), uint8_t(slot)};
            offset += a.descriptor - 1;
        }
    }
    layout.size = offset;
    for (unsigned i = 0; i < layout.fieldCount; ++i) {
        const unsigned width = layout.fields[i].width;
        layout.fieldWidth = i == 0 || layout.fieldWidth == width ? width : 0;
    }
    layout.key = key;
    layout.valid = true;
    return layout;
}
size_t GuestGxStream::VertexSize(unsigned format) {
    return Layout(format).size;
}
std::array<uint8_t, 16> GuestGxStream::ArrayPacket(unsigned slot, ArrayView view) {
    std::array<uint8_t, 16> packet{};
    packet[0] = 0x50;
    packet[2] = 0x10 + slot;
    for (unsigned i = 0; i < 8; ++i)
        packet[3 + i] = uint64_t(view.address) >> (56 - 8 * i);
    for (unsigned i = 0; i < 4; ++i)
        packet[11 + i] = view.size >> (24 - 8 * i);
    return packet;
}
void GuestGxStream::BindDrawArrays(std::span<const uint8_t> draw, const Sink& sink, const ArrayResolver& resolver) {
    const unsigned count = (unsigned(draw[1]) << 8) | draw[2];
    if (!count)
        return;
    const auto& layout = Layout(draw[0] & 7);
    if (!layout.indexedSlots)
        return;
    if (!resolver)
        throw std::runtime_error("Indexed draw requires guest vertex-array resolution");
    if (3 + size_t(count) * layout.size != draw.size())
        throw std::runtime_error("GX array scan disagrees with vertex framing");
    // The largest index each array is read at: a tight pass over the index
    // fields of the cached layout (13,000 draws a frame in the field).
    std::array<uint32_t, 12> maximum{};
    const uint8_t* vertices = draw.data() + 3;
    const auto fields = std::span(layout.fields.data(), layout.fieldCount);
    const bool scanned =
        layout.fieldWidth == 2 ? ScanIndexFields<2>(vertices, count, layout.size, fields, maximum) : layout.fieldWidth == 1 && ScanIndexFields<1>(vertices, count, layout.size, fields, maximum);
    if (!scanned) {
        const uint8_t* vertex = vertices;
        for (unsigned v = 0; v < count; ++v, vertex += layout.size) {
            for (const auto& field : fields) {
                const uint8_t* p = vertex + field.offset;
                const uint32_t index = field.width == 1 ? p[0] : (uint32_t(p[0]) << 8) | p[1];
                maximum[field.slot] = std::max(maximum[field.slot], index);
            }
        }
    }
    for (unsigned slot = 0; slot < 12; ++slot) {
        if (!(layout.indexedSlots & (1u << slot)))
            continue;
        auto& resolved = resolvedArrays[slot];
        if (!resolved.valid || resolved.guest != cp[0xA0 + slot])
            resolved = {cp[0xA0 + slot], resolver(cp[0xA0 + slot]), true};
        auto view = resolved.view;
        // NBT3 carries three independent indices into three-component vectors;
        // ordinary NBT carries one index into a nine-component array element.
        const uint64_t needed = uint64_t(maximum[slot]) * (cp[0xB0 + slot] & 255) + layout.bytes[slot];
        if (!view.address || needed > view.size)
            throw std::runtime_error("GX draw index exceeds guest vertex-array RAM");
        // Rebind only for a new array or one that now needs more bytes. A draw with a
        // smaller maximum index reads a prefix of the bound range, and every rebind
        // makes Aurora upload the array again. Growth at least doubles the range
        // (within the RAM region): meshes drawn in chunks from one big array read
        // further into it draw by draw, and exact growth re-uploaded ever longer
        // prefixes dozens of times per frame.
        auto& bound = boundArrays[slot];
        const bool grow = bound.address == view.address && needed > bound.size;
        if (bound.address != view.address || grow) {
            const uint64_t size = grow && bound.size ? std::max<uint64_t>(needed, uint64_t(bound.size) * 2) : needed;
            view.size = static_cast<uint32_t>(std::min<uint64_t>(size, view.size));
            sink(ArrayPacket(slot, view));
            bound = view;
        }
    }
}

std::array<uint8_t, 37> GuestGxStream::TexturePacket(unsigned slot, const ArrayResolver& resolver) {
    if (!resolver)
        throw std::runtime_error("Guest GX texture requires host pointer conversion");
    const unsigned bank = slot < 4 ? 0x80 : 0xA0;
    const unsigned index = slot & 3;
    const auto image = bp[bank + 8 + index];
    const uint32_t width = (image & 1023) + 1, height = ((image >> 10) & 1023) + 1;
    const uint32_t format = (image >> 20) & 15;
    unsigned tileW = 4, tileH = 4, tileBytes = 32;
    switch (format) {
    case 0:
    case 8:
    case 14:
        tileW = tileH = 8;
        break; // I4, C4, CMPR
    case 1:
    case 2:
    case 9:
        tileW = 8;
        break; // I8, IA4, C8
    case 3:
    case 4:
    case 5:
    case 10:
        break; // IA8, RGB565, RGB5A3, C14X2
    case 6:
        tileBytes = 64;
        break; // RGBA8
    default:
        throw std::runtime_error("Guest GX texture format requires palette/format support: " + std::to_string(format));
    }
    const bool mips = ((bp[bank + index] >> 5) & 3) != 0;
    uint32_t needed = 0, w = width, h = height;
    do {
        needed += ((w + tileW - 1) / tileW) * ((h + tileH - 1) / tileH) * tileBytes;
        if (!mips || (w == 1 && h == 1))
            break;
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    } while (true);
    const uint32_t guest = bp[bank + 20 + index] << 5;
    const auto view = resolver(guest);
    if (view.size < needed)
        throw std::runtime_error("Guest GX texture exceeds its RAM region");
    uint32_t nativeAsset = 0;
#ifdef __vita__
    // Loaded BRRES headers retain their reserved native ID across relocation.
    // Static payloads are read from the offline pack, independent of Wii RAM
    // addresses and their reuse. No content hash or write protection is needed.
    if (guest >= 64 && Memory::Contains(guest - 64, 64)) {
        const uint32_t header = guest - 64;
        const uint32_t tag = Memory::Read32(header + 60);
        if ((tag & 0xf0000000u) == AURORA_NATIVE_TEXTURE_TAG && Memory::Read32(header) == 0x54455830 && Memory::Read32(header + 16) == 64 && Memory::Read16(header + 28) == width &&
            Memory::Read16(header + 30) == height && Memory::Read32(header + 32) == format)
            nativeAsset = tag & 0x0fffffffu;
    }
#endif
#ifdef MH3_HAS_AURORA
    const uint32_t generated = aurora::generated_texture_version(reinterpret_cast<const void*>(view.address), width, height);
#else
    const uint32_t generated = 0;
#endif
    nativeTexture[slot] = nativeAsset != 0 || generated != 0;
    std::array<uint8_t, 37> out{};
    out[0] = 0x50;
    out[2] = 0x30;
    out[3] = slot;
    for (unsigned i = 0; i < 8; ++i)
        out[4 + i] = uint64_t(view.address) >> (56 - 8 * i);
    auto put = [&](unsigned offset, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i)
            out[offset + i] = value >> (24 - 8 * i);
    };
    put(12, width);
    put(16, height);
    put(20, format | (generated ? AURORA_GENERATED_TEXTURE_FORMAT_BIT : nativeAsset ? AURORA_NATIVE_TEXTURE_FORMAT_BIT : 0));
    // Each texture unit gets its own host TLUT slot. Guest TMEM offsets and
    // formats are resolved immediately before the draw that uses the palette.
    if (format >= 8 && format <= 10)
        put(24, slot);
    // Aurora keys its decoded-texture cache on this revision. A fresh number
    // for every BP write defeats that cache; hash the actual guest bytes so a
    // rewrite at the same address still invalidates the decoded texture.
    out[28] = mips;
#ifdef MH3_ENABLE_EFB_COPY_TRACE
    EfbCopyTrace::BeginHostRead(view.address, needed);
#endif
    put(29, guest);
    put(33, generated ? generated : nativeAsset ? nativeAsset : CachedTextureVersion(view.address, needed));
#ifdef MH3_ENABLE_EFB_COPY_TRACE
    EfbCopyTrace::EndHostRead();
#endif
    return out;
}

void GuestGxStream::LoadPalette(const ArrayResolver& resolver) {
    const uint32_t size = ((bp[0x65] >> 10) & 0x7FF) * 32;
    if (!size)
        return;
    if (!resolver)
        throw std::runtime_error("Guest GX TLUT load requires RAM resolution");
    const auto view = resolver(bp[0x64] << 5);
    if (!view.address || size > view.size)
        throw std::runtime_error("Guest GX TLUT load exceeds RAM");
    if (!palettes)
        palettes = std::make_shared<PaletteMemory>();
    const uint32_t destination = (bp[0x65] & 1023) * 512;
    const auto first = palettes->loadedLines.begin() + destination / 32;
    if (std::all_of(first, first + size / 32, [](bool loaded) { return loaded; }) && !std::memcmp(palettes->bytes.data() + destination, reinterpret_cast<const void*>(view.address), size))
        return;
    paletteBindings = {};
    // Preserve the DMA snapshot even if the guest rewrites or frees its source.
    std::memcpy(palettes->bytes.data() + destination, reinterpret_cast<const void*>(view.address), size);
    std::fill_n(palettes->loadedLines.begin() + destination / 32, size / 32, true);
    if (++palettes->revision == 0)
        throw std::runtime_error("Guest GX TLUT revision overflow");
}

uint32_t GuestGxStream::PaletteMemory::ContentsVersion(uint32_t start, uint32_t entries, uint32_t format) {
    const auto* source = bytes.data() + start;
    const size_t size = entries * sizeof(uint16_t);
    const uint64_t digest = ContentsDigest(source, size);
    for (const auto& cached : contents)
        if (cached.version && cached.digest == digest && cached.format == format && cached.bytes.size() == size && !std::memcmp(cached.bytes.data(), source, size))
            return cached.version;
    if (++contentsRevision == 0)
        throw std::runtime_error("Guest GX palette content version overflow");
    auto& cached = contents[nextContents++ % contents.size()];
    cached.digest = digest;
    cached.format = format;
    cached.version = contentsRevision;
    cached.bytes.assign(source, source + size);
    return cached.version;
}

void GuestGxStream::BindDrawPalettes(const Sink& sink, const ArrayResolver& resolver) {
    unsigned used = 0;
    for (unsigned stage = 0; stage <= ((bp[0] >> 10) & 15); ++stage) {
        const auto order = bp[0x28 + stage / 2] >> ((stage & 1) * 12);
        if (order & 64)
            used |= 1u << (order & 7);
    }
    const unsigned indirect = (bp[0] >> 16) & 7;
    if (indirect > 4)
        throw std::runtime_error("Invalid GX indirect stage count");
    for (unsigned stage = 0; stage < indirect; ++stage)
        used |= 1u << ((bp[0x27] >> (stage * 6)) & 7);
    for (unsigned slot = 0; slot < 8; ++slot) {
        if (!(used & (1u << slot)) || !textureBound[slot] || nativeTexture[slot])
            continue;
        const unsigned bank = slot < 4 ? 0x80 : 0xA0, index = slot & 3;
        const unsigned format = (bp[bank + 8 + index] >> 20) & 15;
        if (format < 8 || format > 10)
            continue;
        const uint32_t descriptor = bp[bank + 24 + index] & 0xFFF;
        const unsigned paletteFormat = descriptor >> 10;
        if (paletteFormat > 2)
            throw std::runtime_error("Invalid GX TLUT format");
        const unsigned entries = format == 8 ? 16 : format == 9 ? 256 : 16384;
        const unsigned start = (descriptor & 1023) * 512;
        if (!palettes || !std::all_of(palettes->loadedLines.begin() + start / 32, palettes->loadedLines.begin() + (start + entries * 2) / 32, [](bool loaded) { return loaded; }))
            throw std::runtime_error("Guest GX texture uses an incompletely loaded palette");
        auto& bound = paletteBindings[slot];
        if (bound.revision == palettes->revision && bound.descriptor == descriptor && bound.entries == entries)
            continue;
        std::array<uint8_t, 26> packet{};
        packet[0] = 0x50;
        packet[2] = 0x31;
        packet[3] = slot;
        const auto address = reinterpret_cast<uintptr_t>(palettes->bytes.data() + start);
        for (unsigned i = 0; i < 8; ++i)
            packet[4 + i] = uint64_t(address) >> (56 - 8 * i);
        packet[15] = paletteFormat;
        packet[16] = entries >> 8;
        packet[17] = entries;
        // Stable identity per host slot and exact contents/format. Repeated
        // uploads of an unchanged palette retain the decoded texture in CDRAM.
        const uint32_t identity = 0x80000000u | slot;
        const uint32_t revision = palettes->ContentsVersion(start, entries, paletteFormat);
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

void GuestGxStream::Write(std::span<const uint8_t> bytes, const Sink& sink, const ArrayResolver& resolver, const CopySink& copy, const EfbClearSink& clear) {
    // A 65535-vertex direct packet can exceed 8 MiB with all attributes.
    constexpr size_t limit = 16 * 1024 * 1024;
    if (bytes.size() > limit || pending.size() + bytes.size() > limit)
        throw std::runtime_error("Guest GX command buffer exceeds 16 MiB");
    // The caller's bytes are parsed in place when nothing is pending (display
    // lists); only a command still incomplete at the end waits in `pending`.
    const bool inPlace = pending.empty();
    if (!inPlace)
        pending.insert(pending.end(), bytes.begin(), bytes.end());
    const std::span<const uint8_t> data = inPlace ? bytes : std::span<const uint8_t>(pending);
    // Commands Aurora takes as they are go to the sink in runs, many commands
    // per call: one call per command cost more than decoding them. A command
    // the stream replaces, or precedes with packets of its own, ends a run.
    struct Run {
        const Sink& sink;
        std::span<const uint8_t> data;
        size_t start = 0, at = 0; // [start, at): commands not yet sunk
        void Flush() {
            if (at > start)
                sink(data.subspan(start, at - start));
            start = at;
        }
    } run{sink, data};
    const Sink emit = [&run](std::span<const uint8_t> packet) {
        run.Flush();
        run.sink(packet);
    };
    size_t offset = 0;
    while (offset < data.size()) {
        const auto* p = data.data() + offset;
        const size_t available = data.size() - offset;
        size_t length;
        switch (p[0]) {
        case 0x00:
        case 0x48:
            length = 1;
            break;
        case 0x08:
            length = 6;
            break;
        case 0x61:
            length = 5;
            break;
        case 0x40:
            length = 9;
            break;
        case 0x20:
        case 0x28:
        case 0x30:
        case 0x38:
            length = 5;
            break;
        case 0x10:
            if (available < 5)
                goto incomplete;
            length = 5 + 4 * (1 + ((size_t(p[1]) << 8) | p[2]));
            break;
        default:
            if (p[0] >= 0x80 && p[0] <= 0xBF) {
                if (available < 3)
                    goto incomplete;
                const auto vertices = (size_t(p[1]) << 8) | p[2];
                length = 3 + vertices * VertexSize(p[0] & 7);
                break;
            }
            throw std::runtime_error("Guest GX opcode requires a bridge: " + std::to_string(p[0]));
        }
        if (available < length)
            break;
        run.at = offset;
        ++commandCount;
        if (p[0] == 0x20 || p[0] == 0x28 || p[0] == 0x30 || p[0] == 0x38) {
            if (!resolver)
                throw std::runtime_error("Indexed XF load requires guest RAM resolution");
            const unsigned array = 12 + (p[0] - 0x20) / 8;
            const uint32_t index = (uint32_t(p[1]) << 8) | p[2];
            const unsigned words = (p[3] >> 4) + 1;
            const unsigned destination = ((p[3] & 15) << 8) | p[4];
            // Match Aurora's supported XF memory writes explicitly. Its decoder
            // logs/skips unsupported forms, which must not look successful here.
            const bool supported =
                (destination < 0x78 && destination % 12 == 0 && words == 12) || (destination >= 0x78 && destination < 0xF0 && (destination - 0x78) % 12 == 0 && (words == 8 || words == 12)) ||
                (destination >= 0x400 && destination < 0x45A && (destination - 0x400) % 9 == 0 && words == 9) ||
                (destination >= 0x500 && destination < 0x5F0 && (destination - 0x500) % 12 == 0 && words == 12) || (destination >= 0x600 && destination < 0x680 && (destination & 15) + words <= 16);
            if (!supported)
                throw std::runtime_error("Unsupported indexed XF destination or partial matrix write");
            const uint32_t stride = cp[0xB0 + array] & 255;
            const uint64_t sourceOffset = uint64_t(index) * stride;
            const auto view = resolver(cp[0xA0 + array]);
            if (!stride || !view.address || sourceOffset + words * 4 > view.size)
                throw std::runtime_error("Indexed XF source exceeds guest matrix array");
            std::array<uint8_t, 69> direct{};
            direct[0] = 0x10;
            direct[2] = words - 1;
            direct[3] = destination >> 8;
            direct[4] = destination;
            const auto* source = reinterpret_cast<const uint8_t*>(view.address) + sourceOffset;
            std::copy_n(source, words * 4, direct.begin() + 5);
            emit(std::span<const uint8_t>(direct.data(), 5 + words * 4));
            offset += length;
            run.start = offset;
            continue;
        }
        if (p[0] == 0x40) {
            const uint32_t address = (uint32_t(p[1]) << 24) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 8) | p[4];
            const uint32_t size = (uint32_t(p[5]) << 24) | (uint32_t(p[6]) << 16) | (uint32_t(p[7]) << 8) | p[8];
            if (size) {
                if (!resolver)
                    throw std::runtime_error("GX display list requires guest RAM resolution");
                if (size > limit || displayListDepth >= 16)
                    throw std::runtime_error("GX display list exceeds size or nesting limit");
                const auto view = resolver(address);
                if (!view.address || size > view.size)
                    throw std::runtime_error("GX display list exceeds guest RAM");
                // A separate framer avoids invalidating the parent's partial FIFO
                // packet while processing nested lists. GPU state flows through
                // the call exactly as if its commands appeared inline.
                run.Flush();
                GuestGxStream child;
                child.coalesce = coalesce;
                child.skipDraw = skipDraw;
                child.traceDraw = traceDraw;
                child.displayListAddress = address;
                child.bp = bp;
                child.cp = cp;
                child.bpMask = bpMask;
                child.textureBound = textureBound;
                child.textureDirty = textureDirty;
                child.nativeTexture = nativeTexture;
                child.boundArrays = boundArrays;
                child.palettes = palettes;
                child.paletteBindings = paletteBindings;
                child.displayListDepth = displayListDepth + 1;
                child.Write({reinterpret_cast<const uint8_t*>(view.address), size}, sink, resolver, copy, clear);
                if (child.PendingBytes())
                    throw std::runtime_error("GX display list ends in a partial command");
                bp = child.bp;
                cp = child.cp;
                bpMask = child.bpMask;
                textureBound = child.textureBound;
                textureDirty = child.textureDirty;
                nativeTexture = child.nativeTexture;
                boundArrays = child.boundArrays;
                palettes = child.palettes;
                paletteBindings = child.paletteBindings;
            }
            offset += length;
            run.start = offset;
            continue;
        }
        if (p[0] == 0x08)
            cp[p[1]] = (uint32_t(p[2]) << 24) | (uint32_t(p[3]) << 16) | (uint32_t(p[4]) << 8) | p[5];
        if (p[0] == 0x08 && p[1] >= 0xA0 && p[1] <= 0xAF) {
            if (!resolver)
                throw std::runtime_error("Guest GX vertex-array address requires host pointer conversion");
            const uint32_t guest = (uint32_t(p[2]) << 24) | (uint32_t(p[3]) << 16) | (uint32_t(p[4]) << 8) | p[5];
            const auto view = resolver(guest);
            // Aurora's 0x50/0x0010..1F extension carries a host pointer,
            // accessible byte count, and an explicit guest-big-endian flag.
            emit(ArrayPacket(p[1] - 0xA0, view));
            // Unsized: the next indexed draw binds the range it actually reads.
            boundArrays[p[1] - 0xA0] = {view.address, 0};
            if (p[1] - 0xA0u < resolvedArrays.size())
                resolvedArrays[p[1] - 0xA0] = {guest, view, true};
            offset += length;
            run.start = offset;
            continue;
        }
        if (p[0] == 0x61) {
            const auto reg = p[1];
            const uint32_t value = (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 8) | p[4];
            if (reg == 0xFE)
                bpMask = value;
            else {
                bp[reg] = (bp[reg] & ~bpMask) | (value & bpMask);
                bpMask = 0xFFFFFF;
            }
            if (reg == 0x65)
                LoadPalette(resolver);
            if (reg == 0x66)
                InvalidateTextureHashes();
            const unsigned bank = reg >= 0xA0 ? 0xA0 : 0x80;
            const unsigned group = (reg - bank) & ~3u;
            if (reg >= bank && reg < bank + 28 && (group == 0 || group == 4 || group == 8 || group == 20 || group == 24)) {
                const unsigned slot = (reg & 3) + (bank == 0xA0 ? 4 : 0);
                if (group == 20)
                    textureBound[slot] = true;
                // The texture's metadata (host pointer and contents hash) goes
                // out before the next draw: a texture load writes up to five of
                // these registers, and hashing the texture for each of them was
                // the stream's largest cost.
                if (textureBound[slot])
                    textureDirty |= 1u << slot;
            }
            if (reg == 0x52) {
                const uint32_t trigger = bp[reg];
                if (!(trigger & 0x4000)) {
                    // Texture copies are removed on every backend. Forward
                    // preceding state, but do not resolve guest RAM, invalidate
                    // texture hashes or manufacture a copied texture. Preserve
                    // the command's optional clear as a separate operation.
                    run.at = offset + length;
                    run.Flush();
                    if ((trigger & 0x800) && clear)
                        clear(EfbClear{.x = bp[0x49] & 1023, .y = (bp[0x49] >> 10) & 1023, .sourceWidth = (bp[0x4A] & 1023) + 1, .sourceHeight = ((bp[0x4A] >> 10) & 1023) + 1});
                    offset += length;
                    continue;
                }
                if (!copy || !resolver)
                    throw std::runtime_error("GX display copy requires a host surface bridge");
                const uint32_t width = (bp[0x4D] & 1023) * 16;
                const uint32_t sourceHeight = ((bp[0x4A] >> 10) & 1023) + 1;
                const uint32_t scale = bp[0x4E] & 511;
                // BP bit 10 selects reciprocal scaling, not a scaling enable.
                // Match the hardware copy interval count (height minus one).
                if ((trigger & 0x400) && scale == 0)
                    throw std::runtime_error("GX reciprocal display-copy scale is zero");
                const uint32_t destinationHeight = 1 + ((trigger & 0x400) ? (sourceHeight - 1) * 256 / scale : (sourceHeight - 1) * scale / 256);
                if (width == 0 || width > 1024)
                    throw std::runtime_error("GX display copy has invalid stride");
                const auto view = resolver(bp[0x4B] << 5);
                if (uint64_t(width) * destinationHeight * 2 > view.size)
                    throw std::runtime_error("GX display copy exceeds guest framebuffer RAM");
                run.at = offset + length;
                run.Flush();
                InvalidateTextureHashes();
                copy({view.address, width, destinationHeight, (trigger & 0x800) != 0});
                offset += length;
                continue;
            }
            if (reg == 0x45 || reg == 0x47 || reg == 0x48)
                throw std::runtime_error("Guest GX copy/token/finish requires event integration (BP register " + std::to_string(reg) + ")");
        }
        if (p[0] >= 0x80 && p[0] <= 0xBF) {
            ++drawCount;
            if (skipDraw && skipDraw()) {
                run.Flush();
                ++skippedDrawCount;
                offset += length;
                run.start = offset;
                continue;
            }
            if (traceDraw) {
                run.Flush();
                traceDraw(displayListAddress, displayListAddress ? uint32_t(offset) : 0);
            }
            BindDrawArrays(std::span<const uint8_t>(p, length), emit, resolver);
            for (unsigned slot = 0; slot < 8; ++slot)
                if (textureDirty & (1u << slot))
                    emit(TexturePacket(slot, resolver));
            textureDirty = 0;
            BindDrawPalettes(emit, resolver);
        }
        offset += length;
        if (!coalesce || traceDraw) {
            run.at = offset;
            run.Flush();
        }
    }
incomplete:
    run.at = offset;
    run.Flush();
    if (inPlace)
        pending.assign(data.begin() + offset, data.end());
    else
        pending.erase(pending.begin(), pending.begin() + offset);
}
