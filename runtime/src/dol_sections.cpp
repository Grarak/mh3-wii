// The game's DOL sections, read from the user's disc (sys/main.dol in the
// game data directory) into guest memory at boot. The translated code runs
// natively, but the game reads its own sections: its data, constructor and
// exception tables, and code it inspects. Nothing of the DOL is built into
// the executable.
#include "memory.h"
#include "fatal.h"
#include "../vita/rom_decoder.h"
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace {
// RMHP08's main.dol, the one the code was translated from.
constexpr std::array<uint8_t, 20> kDolSha1{0x7a, 0x12, 0x84, 0xfb, 0xf2, 0xa2, 0x66, 0x40, 0x03, 0x63,
                                           0xaf, 0x75, 0x10, 0xa4, 0x4d, 0x0a, 0xf3, 0xcb, 0x2a, 0x3e};

// A DOL header: 7 text and 11 data sections (file offsets, addresses,
// sizes), then the BSS address and size.
constexpr unsigned kSections = 18;
constexpr size_t kHeaderBytes = 0x100;

bool g_dataInitialized = false;

uint32_t Be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
} // namespace

extern "C" void InitializeDataSections() {
    if (g_dataInitialized)
        return;

    g_dataInitialized = true;
    const char* root = std::getenv("MH3_DISC_DIR");
    const auto path = std::filesystem::path(root ? root : "Assets/DATA") / "sys/main.dol";
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto bytes = file ? file.tellg() : std::streampos(-1);

    if (bytes < std::streampos(kHeaderBytes) || bytes > std::streampos(16 * 1024 * 1024))
        MH3_THROW(std::runtime_error("Missing or invalid game executable " + path.string()));

    std::vector<uint8_t> dol(static_cast<size_t>(bytes));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(dol.data()), dol.size());

    if (!file)
        MH3_THROW(std::runtime_error("Cannot read the game executable " + path.string()));

    std::array<uint8_t, 20> digest;
    mh3_sha1(dol.data(), dol.size(), digest.data());

    if (digest != kDolSha1)
        MH3_THROW(std::runtime_error("The game executable is not the European Monster Hunter Tri (RMHP08) one: " + path.string()));

    for (unsigned i = 0; i < kSections; ++i) {
        const uint32_t offset = Be32(dol.data() + i * 4), address = Be32(dol.data() + 0x48 + i * 4), size = Be32(dol.data() + 0x90 + i * 4);

        if (!size)
            continue;

        if (uint64_t(offset) + size > dol.size() || !Memory::Contains(address, size))
            MH3_THROW(std::runtime_error("Game executable section outside the file or guest memory"));

        std::memcpy(Memory::GetPointer(address, size), dol.data() + offset, size);
    }

    // The BSS stays zero: guest memory starts cleared.
}
