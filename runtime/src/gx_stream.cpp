#include "gx_stream.h"
#include <stdexcept>
#include <array>
#include <string>
#include <algorithm>

std::array<uint8_t, 37> GuestGxStream::TexturePacket(unsigned slot, const ArrayResolver& resolver) {
    if (!resolver) throw std::runtime_error("Guest GX texture requires host pointer conversion");
    const unsigned bank = slot < 4 ? 0x80 : 0xA0;
    const unsigned index = slot & 3;
    const auto image = bp[bank + 8 + index];
    const uint32_t width = (image & 1023) + 1, height = ((image >> 10) & 1023) + 1;
    const uint32_t format = (image >> 20) & 15;
    unsigned tileW = 4, tileH = 4, tileBytes = 32;
    switch (format) {
    case 0: case 14: tileW = tileH = 8; break; // I4, CMPR
    case 1: case 2: tileW = 8; break; // I8, IA4
    case 3: case 4: case 5: break; // IA8, RGB565, RGB5A3
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
    // Non-paletted formats have no TLUT; a fresh revision prevents stale data
    // when the game rewrites a texture at an existing guest address.
    out[28] = mips;
    put(29, guest); put(33, ++textureRevision);
    return out;
}

void GuestGxStream::Write(std::span<const uint8_t> bytes, const Sink& sink, const ArrayResolver& resolver, const CopySink& copy) {
    if (bytes.size() > 1024 * 1024 || pending.size() + bytes.size() > 1024 * 1024)
        throw std::runtime_error("Guest GX command buffer exceeds 1 MiB");
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
        case 0x10:
            if (available < 5) goto incomplete;
            length = 5 + 4 * (1 + ((size_t(p[1]) << 8) | p[2]));
            break;
        default:
            throw std::runtime_error("Guest GX opcode requires a bridge: " + std::to_string(p[0]));
        }
        if (available < length) break;
        if (p[0] == 0x08 && p[1] >= 0xA0 && p[1] <= 0xAF) {
            if (!resolver) throw std::runtime_error("Guest GX vertex-array address requires host pointer conversion");
            const uint32_t guest = (uint32_t(p[2]) << 24) | (uint32_t(p[3]) << 16) | (uint32_t(p[4]) << 8) | p[5];
            const auto view = resolver(guest);
            // Aurora's 0x50/0x0010..1F extension carries a host pointer,
            // accessible byte count, and an explicit guest-big-endian flag.
            std::array<uint8_t, 16> converted{};
            converted[0] = 0x50;
            converted[2] = 0x10 + p[1] - 0xA0;
            for (unsigned i = 0; i < 8; ++i) converted[3 + i] = uint64_t(view.address) >> (56 - 8 * i);
            for (unsigned i = 0; i < 4; ++i) converted[11 + i] = view.size >> (24 - 8 * i);
            sink(converted);
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
            if (reg == 0x64 || reg == 0x65)
                throw std::runtime_error("Guest GX TLUT address requires host pointer conversion");
            const unsigned bank = reg >= 0xA0 ? 0xA0 : 0x80;
            const unsigned group = (reg - bank) & ~3u;
            if (reg >= bank && reg < bank + 24 && (group == 0 || group == 4 || group == 8 || group == 20)) {
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
                if (!(trigger & 0x4000)) throw std::runtime_error("GX texture copy requires a RAM/cache bridge");
                if (!copy || !resolver) throw std::runtime_error("GX display copy requires a host surface bridge");
                const uint32_t width = (bp[0x4D] & 1023) * 16;
                const uint32_t sourceHeight = ((bp[0x4A] >> 10) & 1023) + 1;
                const uint32_t scale = bp[0x4E] & 511;
                // Scaling changes XFB line count. Preserve the exact unscaled
                // path first; other modes remain explicit until validated.
                if (scale != 0 && scale != 256) throw std::runtime_error("GX scaled display copy requires line-count conversion");
                if (width == 0 || width > 1024) throw std::runtime_error("GX display copy has invalid stride");
                const auto view = resolver(bp[0x4B] << 5);
                if (uint64_t(width) * sourceHeight * 2 > view.size)
                    throw std::runtime_error("GX display copy exceeds guest framebuffer RAM");
                sink(std::span<const uint8_t>(p, length));
                copy({view.address, width, sourceHeight, (trigger & 0x800) != 0});
                offset += length;
                continue;
            }
            if (reg == 0x45 || reg == 0x47 || reg == 0x48)
                throw std::runtime_error("Guest GX copy/token/finish requires event integration (BP register " + std::to_string(reg) + ")");
        }
        sink(std::span<const uint8_t>(p, length));
        offset += length;
    }
incomplete:
    pending.erase(pending.begin(), pending.begin() + offset);
}
