#include "abi_bridge.h"
#include "ios_host.h"
#include "video_interface.h"
#include "gx_host.h"
#include "gx_stream.h"
#include <array>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

extern "C" void MH3_EXIInit(CpuContext*);
extern "C" void MH3_EXIGetID(CpuContext*);
extern "C" void MH3_SIInit(CpuContext*);
extern "C" void MH3_SISetXY(CpuContext*);
extern "C" void MH3_SIGetType(CpuContext*);
extern "C" void MH3_OSInitSram(CpuContext*);
extern "C" void MH3_UnlockSram(CpuContext*);
extern "C" void MH3_OSGetResetCode(CpuContext*);
extern "C" void MH3_IOSOpen(CpuContext*);
extern "C" void MH3_IOSClose(CpuContext*);
extern "C" void MH3_IOSRead(CpuContext*);
extern "C" void MH3_IOSWrite(CpuContext*);
extern "C" void MH3_IOSIoctlAsync(CpuContext*);
extern "C" void MH3_IOSIoctlv(CpuContext*);
extern "C" void MH3_IOSOpenAsync(CpuContext*);

namespace {
bool callbackObserved = false;
unsigned callbackDepth = 0, maxCallbackDepth = 0, callbackCount = 0;
void TestIosCallback(CpuContext* cpu) {
    if (static_cast<int32_t>(cpu->gpr[3]) != -106 || cpu->gpr[4] != 0x12345678)
        throw std::runtime_error("IOS callback arguments were not delivered");
    callbackObserved = true;
    ++callbackCount;
    ++callbackDepth;
    maxCallbackDepth = std::max(maxCallbackDepth, callbackDepth);
    PumpIosCompletions(cpu);
    --callbackDepth;
    cpu->gpr[14] = 0xDEADBEEF;
    cpu->lr = 0;
}
}

void RunDeviceSelfTests(CpuContext* cpu) {
    for (uint32_t channel = 0; channel < 4; ++channel) {
        cpu->gpr[3] = channel;
        Memory::Write32(0x8062A8B8 + channel * 4, 0x80);
        MH3_SIGetType(cpu);
        if (cpu->gpr[3] != 8 || Memory::Read32(0x8062A8B8 + channel * 4) != 8)
            throw std::runtime_error("SI absent-device result/state failed");
    }
    // The locked cache is independent storage, including at its last byte.
    Memory::Write32(0xE0000000, 0x12345678);
    MemoryInline::FlatWriteRam32(0xE0003FFC, 0xABCDEF01);
    if (Memory::Read32(0xE0000000) != 0x12345678 ||
        Memory::Read32(0xE0003FFC) != 0xABCDEF01 ||
        Memory::Contains(0xE0003FFC, 8) || Memory::Contains(0xE0004000, 1) ||
        Memory::Contains(0x20000000, 4) || Memory::Contains(0xA0000000, 4))
        throw std::runtime_error("Locked-cache mapping/bounds failed");
    if (Memory::Read16(0xCC002002) != 0x0101)
        throw std::runtime_error("VI initial PAL mode failed");
    Memory::Write16(0xCC002048, 0x2828);
    if (Memory::Read16(0x0C002048) != 0x2828)
        throw std::runtime_error("VI register alias/storage failed");
    const uint16_t line = Memory::Read16(0xCC00202C);
    const uint16_t sample = Memory::Read16(0xCC00202E);
    if (line < 1 || line > 625 || sample < 1 || sample > 864)
        throw std::runtime_error("VI beam position out of PAL bounds");
    BeginVideoRetrace();
    if (Memory::Read16(0xCC00202C) != 1 || !(Memory::Read16(0xCC002030) & 0x8000) || VideoRetracePeriodNs() != 20000000)
        throw std::runtime_error("VI first PAL retrace latch failed");
    Memory::Write16(0xCC002030, Memory::Read16(0xCC002030) & 0x7FFF);
    EndVideoRetrace();
    BeginVideoRetrace();
    if (Memory::Read16(0xCC00202C) != 313 || !(Memory::Read16(0xCC002034) & 0x8000) || (Memory::Read16(0xCC002030) & 0x8000))
        throw std::runtime_error("VI second-field/acknowledgement failed");
    EndVideoRetrace();
    Memory::Write16(0xCC002002, 5); // enabled, non-interlaced NTSC
    BeginVideoRetrace(); EndVideoRetrace(); BeginVideoRetrace();
    if (Memory::Read16(0xCC00202C) != 1 || VideoRetracePeriodNs() != 16683333)
        throw std::runtime_error("VI non-interlaced retrace failed");
    EndVideoRetrace();
    ResetVideoInterface();
    uint16_t unused;
    if (ReadVideoRegister16(0xCC003000, unused) || ReadVideoRegister16(0xCC002001, unused))
        throw std::runtime_error("VI intercepted an unrelated or unaligned register");
    Memory::Write16(0xCC000020, 0xC600);
    Memory::Write32(0xCC00300C, 0x00E2C600);
    if (Memory::Read16(0x0C000020) != 0xC600 || Memory::Read32(0x8C00300C) != 0x00E2C600)
        throw std::runtime_error("GX FIFO register aliases failed");
    uint32_t gxValue;
    if (ReadGxRegister(0xCC000000, 2, gxValue) || ReadGxRegister(0xCC000021, 2, gxValue) ||
        ReadGxRegister(0xCC00300C, 2, gxValue) || ReadGxRegister(0x4C00300C, 4, gxValue))
        throw std::runtime_error("GX register interface accepted an unsupported access");
    Memory::Write16(0xCC000006, 0xFFFF);
    Memory::Write16(0xCC000022, 0xFFFF);
    Memory::Write16(0xCC000004, 3);
    if (Memory::Read16(0xCC000006) != 7 || Memory::Read16(0xCC000022) != 0x1FFF || Memory::Read16(0xCC000004))
        throw std::runtime_error("GX CP register masking/acknowledgement failed");
    ResetGxRegisters();
    GuestGxStream stream;
    std::vector<uint8_t> delivered;
    unsigned packets = 0;
    auto sink = [&](std::span<const uint8_t> packet) {
        ++packets;
        delivered.insert(delivered.end(), packet.begin(), packet.end());
    };
    const std::array<uint8_t, 15> commands{0x08, 0x50, 0, 0, 0, 1, 0x10, 0, 0, 0x10, 0, 0x3F, 0x80, 0, 0};
    stream.Write(std::span(commands).first(5), sink);
    if (packets || stream.PendingBytes() != 5) throw std::runtime_error("GX decoded a partial command");
    stream.Write(std::span(commands).subspan(5, 5), sink);
    if (packets != 1 || stream.PendingBytes() != 4) throw std::runtime_error("GX command framing failed");
    stream.Write(std::span(commands).subspan(10), sink);
    if (packets != 2 || stream.PendingBytes() || !std::equal(delivered.begin(), delivered.end(), commands.begin(), commands.end()))
        throw std::runtime_error("GX stream changed command bytes");
    bool rejected = false;
    try { const uint8_t command[]{0x08, 0xA0, 0x80, 0, 0, 0}; stream.Write(command, sink); }
    catch (const std::runtime_error&) { rejected = true; }
    if (!rejected || packets != 2) throw std::runtime_error("GX forwarded an unconverted guest pointer");
    GuestGxStream translatedStream;
    delivered.clear();
    const uint8_t arrayCommand[]{0x08, 0xA3, 0x10, 0, 0x12, 0x34};
    translatedStream.Write(arrayCommand, sink, [](uint32_t guest) -> GuestGxStream::ArrayView {
        if (guest != 0x10001234) throw std::runtime_error("GX array address byte order failed");
        return {0x0123456789ABCDEFULL, 0x1234};
    });
    const std::vector<uint8_t> expected{0x50, 0, 0x13, 1, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0, 0, 0x12, 0x34, 0};
    if (delivered != expected) throw std::runtime_error("GX host array command encoding failed");
    auto textureCase = [&](uint32_t format, uint32_t width, uint32_t height, bool mips, uint32_t bytes, bool shouldPass) {
        GuestGxStream textureStream;
        std::vector<uint8_t> metadata;
        auto textureSink = [&](std::span<const uint8_t> packet) {
            if (packet[0] == 0x50) metadata.assign(packet.begin(), packet.end());
        };
        auto resolve = [&](uint32_t address) -> GuestGxStream::ArrayView {
            if (address != 0x100000) throw std::runtime_error("GX texture address scaling failed");
            return {0x0123456789ABCDEFULL, bytes};
        };
        auto bpWrite = [&](uint8_t reg, uint32_t value) {
            const uint8_t command[]{0x61, reg, uint8_t(value >> 16), uint8_t(value >> 8), uint8_t(value)};
            textureStream.Write(command, textureSink, resolve);
        };
        bool passed = true;
        try {
            bpWrite(0x80, mips ? 0xC0 : 0x80);
            bpWrite(0x88, (format << 20) | ((height - 1) << 10) | (width - 1));
            // A masked width update must preserve format/height, and its mask
            // must not leak into the following image-address write.
            bpWrite(0xFE, 0x3FF);
            bpWrite(0x88, 0xF00000 | (width - 1));
            bpWrite(0x94, 0x100000 >> 5);
        } catch (const std::runtime_error&) { passed = false; }
        if (passed != shouldPass) throw std::runtime_error("GX texture footprint validation failed");
        if (passed && (metadata.size() != 37 || metadata[2] != 0x30 || metadata[4] != 1 ||
            metadata[11] != 0xEF || metadata[15] != width || metadata[19] != height ||
            metadata[23] != format || metadata[28] != mips))
            throw std::runtime_error("GX texture metadata encoding failed");
    };
    textureCase(4, 4, 4, false, 32, true);
    textureCase(6, 5, 5, false, 255, false);
    textureCase(6, 5, 5, false, 256, true);
    textureCase(14, 9, 9, false, 128, true);
    textureCase(6, 4, 4, true, 191, false);
    textureCase(6, 4, 4, true, 192, true);
    textureCase(8, 4, 4, false, 1024, false);
    auto copyCase = [&](uint32_t room, uint32_t scale, uint32_t trigger, bool shouldPass) {
        GuestGxStream copyStream;
        unsigned copies = 0;
        auto resolveCopy = [&](uint32_t address) -> GuestGxStream::ArrayView {
            if (address != 0x200000) throw std::runtime_error("GX copy address scaling failed");
            return {0x12345678, room};
        };
        auto copied = [&](const GuestGxStream::DisplayCopy& copy) {
            if (copy.destination != 0x12345678 || copy.width != 640 || copy.height != 528 || !copy.clear)
                throw std::runtime_error("GX copy metadata incorrect");
            ++copies;
        };
        auto writeBp = [&](uint8_t reg, uint32_t value) {
            const uint8_t command[]{0x61, reg, uint8_t(value >> 16), uint8_t(value >> 8), uint8_t(value)};
            copyStream.Write(command, [](auto) {}, resolveCopy, copied);
        };
        bool passed = true;
        try {
            writeBp(0x4A, (527 << 10) | 639);
            writeBp(0x4B, 0x200000 >> 5);
            writeBp(0x4D, 40);
            writeBp(0x4E, scale);
            writeBp(0x52, trigger);
        } catch (const std::runtime_error&) { passed = false; }
        if (passed != shouldPass || copies != unsigned(shouldPass)) throw std::runtime_error("GX display-copy bounds/dispatch failed");
    };
    copyCase(640 * 528 * 2, 256, 0x4800, true);
    copyCase(640 * 528 * 2 - 1, 256, 0x4800, false);
    copyCase(640 * 528 * 2, 128, 0x4800, false);
    copyCase(640 * 528 * 2, 256, 0x0800, false);
    MH3_OSGetResetCode(cpu);
    if (cpu->gpr[3] != 0) throw std::runtime_error("Cold boot reset code failed");
    Memory::Write32(0x8074F740, 1);
    Memory::Write32(0x8074F744, 0x1234);
    MH3_OSGetResetCode(cpu);
    if (cpu->gpr[3] != 0x80001234) throw std::runtime_error("Saved reset code failed");
    Memory::Write32(0x8074F740, 0);
    MH3_EXIInit(cpu);
    cpu->gpr[5] = 0x80001100;
    Memory::Write32(cpu->gpr[5], 0xFFFFFFFF);
    MH3_EXIGetID(cpu);
    if (cpu->gpr[3] != 0 || Memory::Read32(cpu->gpr[5]) != 0 ||
        Memory::Read32(0x80003040 + 9 * 4) != 0x804B1310)
        throw std::runtime_error("EXI absence/interrupt-state test failed");
    MH3_SIInit(cpu);
    Memory::Write32(0x8062A8A4, 0xFC123411);
    cpu->gpr[3] = 0x70; cpu->gpr[4] = 3;
    MH3_SISetXY(cpu);
    if (cpu->gpr[3] != 0xFC700311 || Memory::Read32(0x8062A8A4) != 0xFC700311)
        throw std::runtime_error("SI polling-rate guest state failed");
    Memory::Write32(0x8062A8A4, 0);
    if (Memory::Read32(0x80797934) != 1 || Memory::Read32(0x80759B28) != 0xFFFFFFFF)
        throw std::runtime_error("SI initialization test failed");

    // Never read or modify the user's actual saved settings during tests.
    struct TestDirectory {
        std::filesystem::path path;
        bool hadOriginal;
        std::string original;
        TestDirectory() {
            const char* value = std::getenv("MH3_USER_DIR");
            hadOriginal = value != nullptr;
            if (value) original = value;
            std::string pattern = (std::filesystem::temp_directory_path() / "mh3-sram-test-XXXXXX").string();
            if (!mkdtemp(pattern.data())) throw std::runtime_error("Unable to create SRAM test directory");
            path = pattern;
            setenv("MH3_USER_DIR", pattern.c_str(), 1);
        }
        ~TestDirectory() {
            if (hadOriginal) setenv("MH3_USER_DIR", original.c_str(), 1);
            else unsetenv("MH3_USER_DIR");
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    } directory;
    MH3_OSInitSram(cpu);
    if (Memory::Read8(0x8074FB13) != 0x0C || Memory::Read32(0x8074FB4C) != 1)
        throw std::runtime_error("Default SRAM test failed");
    Memory::Write8(0x8074FB12, 2);
    Memory::Write32(0x8074FB44, 1);
    cpu->gpr[3] = 1;
    cpu->gpr[4] = 0;
    MH3_UnlockSram(cpu);
    if (!(cpu->msr & 0x8000) || Memory::Read32(0x8074FB48) != 0)
        throw std::runtime_error("SRAM unlock/interrupt-restore test failed");
    Memory::Write8(0x8074FB12, 0);
    MH3_OSInitSram(cpu);
    if (Memory::Read8(0x8074FB12) != 2 || Memory::Read16(0x8074FB00) != 0x020C ||
        Memory::Read16(0x8074FB02) != 0xFDF0)
        throw std::runtime_error("SRAM persistence/checksum test failed");

    const auto nand = directory.path / "nand";
    std::filesystem::create_directories(nand / "shared2");
    { std::ofstream file(nand / "shared2/settings"); file << "ABCD"; }
    auto open = [&](const std::string& path, uint32_t mode) {
        for (size_t i = 0; i <= path.size(); ++i)
            Memory::Write8(0x80001200 + i, i == path.size() ? 0 : path[i]);
        cpu->gpr[3] = 0x80001200; cpu->gpr[4] = mode;
        MH3_IOSOpen(cpu);
        return static_cast<int32_t>(cpu->gpr[3]);
    };
    int32_t fd = open("/shared2/settings", 3);
    if (fd < 0) throw std::runtime_error("IOS file open failed");
    cpu->gpr[3] = fd; cpu->gpr[4] = 0x80001300; cpu->gpr[5] = 2;
    MH3_IOSRead(cpu);
    if (cpu->gpr[3] != 2 || Memory::Read16(0x80001300) != 0x4142)
        throw std::runtime_error("IOS file read failed");
    Memory::Write16(0x80001300, 0x5859);
    cpu->gpr[3] = fd; cpu->gpr[4] = 0x80001300; cpu->gpr[5] = 2;
    MH3_IOSWrite(cpu);
    if (cpu->gpr[3] != 2) throw std::runtime_error("IOS file write failed");
    cpu->gpr[3] = fd; MH3_IOSClose(cpu);
    { std::ifstream file(nand / "shared2/settings"); std::string text; file >> text;
      if (text != "ABXY") throw std::runtime_error("IOS file persistence failed"); }
    fd = open("/shared2/settings", 1);
    if (fd < 0) throw std::runtime_error("IOS read-only open failed");
    cpu->gpr[3] = fd; cpu->gpr[4] = 0x80001300; cpu->gpr[5] = 2;
    MH3_IOSWrite(cpu);
    if (static_cast<int32_t>(cpu->gpr[3]) != -102)
        throw std::runtime_error("IOS read-only mode was not enforced");
    cpu->gpr[3] = fd; MH3_IOSClose(cpu);
    if (open("/../escape", 1) != -4 || open("/missing", 1) != -106)
        throw std::runtime_error("IOS path/error handling failed");
    std::filesystem::create_directory_symlink(directory.path, nand / "link");
    if (open("/link/sram.bin", 1) >= 0)
        throw std::runtime_error("IOS NAND symlink isolation failed");
    fd = open("/dev/stm/immediate", 0);
    cpu->gpr[3] = fd; cpu->gpr[4] = 0x5001;
    cpu->gpr[5] = 0x80001400; cpu->gpr[6] = 20;
    cpu->gpr[7] = 0; cpu->gpr[8] = 0;
    cpu->gpr[9] = 0x804D6320; cpu->gpr[10] = 0xA5;
    MH3_IOSIoctlAsync(cpu);
    auto dimming = TakeIosCompletion();
    if (cpu->gpr[3] || !dimming || dimming->result || dimming->callback != 0x804D6320 || dimming->argument != 0xA5)
        throw std::runtime_error("STM async dimming completion failed");
    cpu->gpr[3] = fd; MH3_IOSClose(cpu);
    fd = open("/dev/stm/eventhook", 0);
    if (fd < 0) throw std::runtime_error("STM event device open failed");
    Memory::Write32(0x80001400, 0xDEADBEEF);
    cpu->gpr[3] = fd; cpu->gpr[4] = 0x1000;
    cpu->gpr[7] = 0x80001400; cpu->gpr[8] = 0x20;
    cpu->gpr[9] = 0x804D6320; cpu->gpr[10] = 0;
    MH3_IOSIoctlAsync(cpu);
    if (cpu->gpr[3] || Memory::Read32(0x80001400) != 0xDEADBEEF)
        throw std::runtime_error("STM event subscription must remain pending");
    cpu->gpr[3] = fd;
    MH3_IOSIoctlAsync(cpu);
    auto completion = TakeIosCompletion();
    if (cpu->gpr[3] || !completion || completion->result != -8)
        throw std::runtime_error("STM duplicate subscription completion failed");
    TriggerStmEvent(0x20000);
    completion = TakeIosCompletion();
    if (!completion || completion->callback != 0x804D6320 || completion->result ||
        Memory::Read32(0x80001400) != 0x20000 || TakeIosCompletion())
        throw std::runtime_error("STM event delivery failed");
    cpu->gpr[3] = fd; MH3_IOSClose(cpu);
    fd = open("/dev/es", 0);
    Memory::Write32(0x80001500, 0x80001600);
    Memory::Write32(0x80001504, 8);
    cpu->gpr[3] = fd; cpu->gpr[4] = 0x20;
    cpu->gpr[5] = 0; cpu->gpr[6] = 1; cpu->gpr[7] = 0x80001500;
    MH3_IOSIoctlv(cpu);
    if (cpu->gpr[3] || Memory::Read64(0x80001600) != 0x00010000524D4850ull)
        throw std::runtime_error("ES title identity failed");
    Memory::Write32(0x80001508, 0x80001700);
    Memory::Write32(0x8000150C, 30);
    cpu->gpr[3] = fd; cpu->gpr[4] = 0x1D;
    cpu->gpr[5] = 1; cpu->gpr[6] = 1; cpu->gpr[7] = 0x80001500;
    MH3_IOSIoctlv(cpu);
    if (cpu->gpr[3] || std::string(reinterpret_cast<char*>(Memory::GetPointer(0x80001700, 30))) !=
        "/title/00010000/524d4850/data")
        throw std::runtime_error("ES data directory failed");
    for (uint32_t i = 0; i < 0x2A4; ++i) Memory::Write8(0x80001800 + i, 0);
    Memory::Write32(0x80001800, 0x00010001);
    for (uint32_t i = 0; i < 0xD4; ++i) Memory::Write8(0x800019D0 + i, i);
    Memory::Write32(0x80001500, 0x80001800);
    Memory::Write32(0x80001504, 0x2A4);
    Memory::Write32(0x80001508, 0x80001B00);
    Memory::Write32(0x8000150C, 0xD8);
    cpu->gpr[3] = fd; cpu->gpr[4] = 0x1B;
    cpu->gpr[5] = 1; cpu->gpr[6] = 1; cpu->gpr[7] = 0x80001500;
    MH3_IOSIoctlv(cpu);
    if (cpu->gpr[3] || Memory::Read32(0x80001B00))
        throw std::runtime_error("ES ticket view header failed");
    for (uint32_t i = 0; i < 0xD4; ++i)
        if (Memory::Read8(0x80001B04 + i) != i)
            throw std::runtime_error("ES ticket view contents failed");
    cpu->gpr[3] = fd; MH3_IOSClose(cpu);
    // A missing file is an asynchronous operation result, not a queue failure.
    open("/missing", 1); // also writes the test path into guest memory
    cpu->gpr[3] = 0x80001200; cpu->gpr[4] = 1;
    cpu->gpr[5] = 0x804DB9B0; cpu->gpr[6] = 0x12345678;
    MH3_IOSOpenAsync(cpu);
    auto fileCompletion = TakeIosCompletion();
    if (cpu->gpr[3] || !fileCompletion || fileCompletion->result != -106 ||
        fileCompletion->callback != 0x804DB9B0 || fileCompletion->argument != 0x12345678)
        throw std::runtime_error("IOS asynchronous error completion failed");
    // Test-only registration; never included in the production guest map.
    AbiTrampoline<decltype(TestIosCallback)> testCallback(
        0x81700000, "IOS callback test", TestIosCallback, FunctionKind::Native,
        false, 0, 0, 0, TestIosCallback);
    TranslatedFunctionRegistry::Finalize();
    for (unsigned i = 0; i < 2; ++i) {
        cpu->gpr[3] = 0x80001200; cpu->gpr[4] = 1;
        cpu->gpr[5] = 0x81700000; cpu->gpr[6] = 0x12345678;
        MH3_IOSOpenAsync(cpu);
    }
    cpu->gpr[14] = 0x1234; cpu->lr = 0x5678;
    PumpIosCompletions(cpu);
    if (!callbackObserved || callbackCount != 2 || maxCallbackDepth != 1 || cpu->gpr[14] != 0x1234 || cpu->lr != 0x5678 || TakeIosCompletion())
        throw std::runtime_error("IOS callback CPU-state preservation failed");
}
