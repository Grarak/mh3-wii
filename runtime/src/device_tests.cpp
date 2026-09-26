#include "abi_bridge.h"
#include "ios_host.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

extern "C" void MH3_EXIInit(CpuContext*);
extern "C" void MH3_EXIGetID(CpuContext*);
extern "C" void MH3_SIInit(CpuContext*);
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
void TestIosCallback(CpuContext* cpu) {
    if (static_cast<int32_t>(cpu->gpr[3]) != -106 || cpu->gpr[4] != 0x12345678)
        throw std::runtime_error("IOS callback arguments were not delivered");
    callbackObserved = true;
    cpu->gpr[14] = 0xDEADBEEF;
    cpu->lr = 0;
}
}

void RunDeviceSelfTests(CpuContext* cpu) {
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
    cpu->gpr[3] = 0x80001200; cpu->gpr[4] = 1;
    cpu->gpr[5] = 0x81700000; cpu->gpr[6] = 0x12345678;
    MH3_IOSOpenAsync(cpu);
    cpu->gpr[14] = 0x1234; cpu->lr = 0x5678;
    PumpIosCompletions(cpu);
    if (!callbackObserved || cpu->gpr[14] != 0x1234 || cpu->lr != 0x5678 || TakeIosCompletion())
        throw std::runtime_error("IOS callback CPU-state preservation failed");
}
