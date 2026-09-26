#include "abi_bridge.h"
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <unistd.h>

extern "C" void MH3_EXIInit(CpuContext*);
extern "C" void MH3_EXIGetID(CpuContext*);
extern "C" void MH3_SIInit(CpuContext*);
extern "C" void MH3_OSInitSram(CpuContext*);
extern "C" void MH3_UnlockSram(CpuContext*);
extern "C" void MH3_OSGetResetCode(CpuContext*);

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
}
