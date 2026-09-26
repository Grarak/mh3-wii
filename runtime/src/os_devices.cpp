#include "abi_bridge.h"
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {
constexpr uint32_t kSram = 0x8074FB00;
std::filesystem::path SramPath() {
    const char* user = std::getenv("MH3_USER_DIR");
    return (user ? std::filesystem::path(user) : std::filesystem::path("saves")) / "sram.bin";
}
void FixSramChecksum() {
    uint16_t sum = 0, inverse = 0;
    for (uint32_t offset = 0xC; offset < 0x14; offset += 2) {
        uint16_t value = Memory::Read16(kSram + offset);
        sum += value;
        inverse += static_cast<uint16_t>(~value);
    }
    Memory::Write16(kSram, sum);
    Memory::Write16(kSram + 2, inverse);
}
void SaveSram() {
    const auto path = SramPath();
    std::filesystem::create_directories(path.parent_path());
    auto staging = path;
    staging += ".tmp";
    {
        std::ofstream file(staging, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(Memory::GetPointer(kSram, 0x40)), 0x40);
        file.close();
        if (!file) throw std::runtime_error("Unable to persist host SRAM");
    }
    std::filesystem::rename(staging, path);
}
}

// EXIInit: no GameCube memory cards or debug adapters are connected. Device
// probing must report absence rather than fabricate a successful bus transfer.
// The RTC/SRAM device is supplied by the dedicated host SRAM service below.
extern "C" void MH3_EXIInit(CpuContext*) {
    Memory::Write32(0x800000C4, Memory::Read32(0x800000C4) | 0x007F8000);
    constexpr std::array<uint32_t, 8> handlers = {
        0x804B1310, 0x804B13D0, 0x804B15F0,
        0x804B1310, 0x804B13D0, 0x804B15F0, 0x804B1310, 0x804B13D0};
    for (unsigned i = 0; i < handlers.size(); ++i)
        Memory::Write32(0x80003040 + (9 + i) * 4, handlers[i]);
    // Probe timestamps and device-2 ID cache used by the original EXI library.
    Memory::Write32(0x800030C0, 0);
    Memory::Write32(0x800030C4, 0);
    Memory::Write32(0x80749040, 0);
    Memory::Write32(0x80749080, 0);
    Memory::Write32(0x80797658, 0);
}
REGISTER_NATIVE_FUNCTION(0x804B16C0, MH3_EXIInit);

extern "C" void MH3_EXIProbe(CpuContext* ctx) { ctx->gpr[3] = 0; }
REGISTER_NATIVE_FUNCTION(0x804B0D80, MH3_EXIProbe);
extern "C" void MH3_EXIGetID(CpuContext* ctx) {
    if (ctx->gpr[5]) Memory::Write32(ctx->gpr[5], 0);
    ctx->gpr[3] = 0;
}
REGISTER_NATIVE_FUNCTION(0x804B1AA0, MH3_EXIGetID);

// SIInit: initialize the original four transfer queues, channel state and
// interrupt binding. Controller input will use host PAD/WPAD services.
extern "C" void MH3_SIInit(CpuContext*) {
    if (Memory::Read32(0x80797934)) return;
    for (uint32_t i = 0; i < 4; ++i) Memory::Write32(0x80759AC8 + i * 0x20, 0xFFFFFFFF);
    Memory::Write32(0x8062A8A4, 0);
    Memory::Write32(0x80003040 + 20 * 4, 0x804DE3A0);
    Memory::Write32(0x800000C4, Memory::Read32(0x800000C4) & ~0x800u);
    Memory::Write32(0x80797934, 1);
}
REGISTER_NATIVE_FUNCTION(0x804DE900, MH3_SIInit);

// __OSInitSram. SRAM settings are a 64-byte big-endian image. Layout and
// checksum are verified against MH3's unlock routine at 0x804D3550; see also
// Dolphin's Source/Core/Core/HW/Sram.h for field descriptions.
extern "C" void MH3_OSInitSram(CpuContext*) {
    std::memset(Memory::GetPointer(kSram, 0x50), 0, 0x50);
    const auto path = SramPath();
    if (std::filesystem::exists(path)) {
        if (std::filesystem::file_size(path) != 0x40)
            throw std::runtime_error("Host SRAM file must contain exactly 64 bytes");
        std::ifstream file(path, std::ios::binary);
        file.read(reinterpret_cast<char*>(Memory::GetPointer(kSram, 0x40)), 0x40);
        if (!file) throw std::runtime_error("Unable to read host SRAM");
    } else {
        Memory::Write8(kSram + 0x12, 0); // English
        Memory::Write8(kSram + 0x13, 0x0C); // stereo, setup complete
        FixSramChecksum();
    }
    Memory::Write32(kSram + 0x40, 0x40); // no dirty bytes
    Memory::Write32(kSram + 0x4C, 1); // synchronized
}
REGISTER_NATIVE_FUNCTION(0x804D3350, MH3_OSInitSram);

// Common UnlockSram(commit, offset), called by the translated lock/unlock APIs.
// The original EXI DMA is replaced by a synchronous atomic host-file update.
extern "C" void MH3_UnlockSram(CpuContext* ctx) {
    if (ctx->gpr[3]) {
        if (ctx->gpr[4] == 0) {
            uint8_t flags = Memory::Read8(kSram + 0x13);
            if ((flags & 3) == 3) Memory::Write8(kSram + 0x13, flags & ~3u);
            FixSramChecksum();
        }
        const uint16_t mode = Memory::Read16(kSram + 0x3C);
        if ((mode & 0x7C00) == 0x5000 || (mode & 0xC0) == 0xC0)
            Memory::Write16(kSram + 0x3C, 0);
        SaveSram();
    }
    Memory::Write32(kSram + 0x40, 0x40);
    Memory::Write32(kSram + 0x48, 0);
    Memory::Write32(kSram + 0x4C, 1);
    if (Memory::Read32(kSram + 0x44)) ctx->msr |= 0x8000;
    else ctx->msr &= ~0x8000u;
    ctx->gpr[3] = 1;
}
REGISTER_NATIVE_FUNCTION(0x804D3550, MH3_UnlockSram);

// __OSInitAudioSystem uploads a temporary DSP bootstrap and waits for the
// physical DSP to acknowledge it, restoring its scratch RAM before returning.
// Native execution has no DSP to reset. This only replaces hardware bring-up;
// AX mixing and audio output still require their own host implementation.
extern "C" void MH3_OSInitAudioSystem(CpuContext*) {}
REGISTER_NATIVE_FUNCTION(0x804CC9D0, MH3_OSInitAudioSystem);
