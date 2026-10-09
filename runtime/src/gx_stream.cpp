#include "gx_stream.h"
#include <stdexcept>
#include <array>
#include <string>

void GuestGxStream::Write(std::span<const uint8_t> bytes, const Sink& sink, const ArrayResolver& resolver) {
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
            if ((reg >= 0x94 && reg <= 0x97) || (reg >= 0xB4 && reg <= 0xB7) || reg == 0x64 || reg == 0x65)
                throw std::runtime_error("Guest GX texture/TLUT address requires host pointer conversion");
            if (reg == 0x52 || reg == 0x45 || reg == 0x47 || reg == 0x48)
                throw std::runtime_error("Guest GX copy/token/finish requires event integration");
        }
        sink(std::span<const uint8_t>(p, length));
        offset += length;
    }
incomplete:
    pending.erase(pending.begin(), pending.begin() + offset);
}
