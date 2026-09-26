#include "abi_bridge.h"
#include "generated/RuntimeConfig.h"
#include <cstdlib>
#include <execinfo.h>
#include <iostream>
#include <string_view>

extern "C" void InitializeDataSections();
extern "C" void MH3_OSExceptionInit(CpuContext*);
extern "C" void MH3_OSInterruptInit(CpuContext*);

void SystemBridge::DumpCpuState(std::ostream& out, const CpuContext* cpu) {
    if (!cpu) return;
    out << std::hex << "guest pc=" << cpu->pc << " lr=" << cpu->lr << " ctr=" << cpu->ctr << '\n';
    for (unsigned i = 0; i < 32; ++i) out << "r" << std::dec << i << "=" << std::hex << cpu->gpr[i] << ((i % 4 == 3) ? '\n' : ' ');
    out << std::dec;
}
void SystemBridge::DumpCpuState(const CpuContext* cpu) { DumpCpuState(std::cerr, cpu); }
void DumpHostStackTraceForRuntimeHelper() {
    void* frames[40];
    int count = backtrace(frames, 40);
    backtrace_symbols_fd(frames, count, 2);
}
void ShowRuntimeFatalPopup(std::string_view category, std::string_view details) noexcept {
    std::cerr << category << ": " << details << '\n';
}
void MarkFatalErrorReported() {}
void SetRuntimeExitCode(int) {}
namespace RuntimeCrash {
void WriteCrashArtifacts(std::string_view reason, std::string_view details, const uint32_t*) noexcept {
    std::cerr << reason << ": " << details << '\n';
    SystemBridge::DumpCpuState(TryGetCpuContext());
}
[[noreturn]] void FatalMissingGuestTarget(uint32_t target, CpuContext* cpu) noexcept {
    std::cerr << "Missing translated target 0x" << std::hex << target << std::dec << '\n';
    SystemBridge::DumpCpuState(cpu);
    DumpHostStackTraceForRuntimeHelper();
    std::exit(1);
}
}

int main(int argc, char** argv) {
    try {
        Memory::Init(24 * 1024 * 1024);
        InitializePersistentCpuContext();
        auto& cpu = GetPersistentCpuContext();
        CpuContextScope scope(&cpu);
        if (argc == 2 && std::string_view(argv[1]) == "--self-test") {
            Memory::Write32(0x80001000, 0x12345678);
            if (Memory::Read32(0x1000) != 0x12345678 || Memory::Read16(0xC0001000) != 0x1234)
                throw std::runtime_error("RAM alias or big-endian access failed");
            MemoryInline::FlatWriteRam32(0x80001004, 0xABCDEF01);
            if (MemoryInline::FlatLoad<uint32_t>(0xC0001004) != 0xABCDEF01)
                throw std::runtime_error("Translated stack-store mapping failed");
            bool caught = false;
            try { Memory::Read32(0xCC000000); } catch (const Memory::AccessViolation&) { caught = true; }
            if (!caught) throw std::runtime_error("MMIO must not silently read as RAM");
            MH3_OSExceptionInit(&cpu);
            if (Memory::Read32(0x80797798) != 0x80003000 || Memory::Read32(0x80003038) != 0x804CB930)
                throw std::runtime_error("MH3 exception table initialization failed");
            Memory::Write32(0x800030BC, 0xFFFFFFFF);
            MH3_OSInterruptInit(&cpu);
            if (Memory::Read32(0x80797828) != 0x80003040 || Memory::Read32(0x800030BC) != 0 ||
                Memory::Read32(0x800000C4) != 0xFFFFFFF0 || Memory::Read32(0x80003010) != 0x804D1A70)
                throw std::runtime_error("MH3 interrupt initialization failed");
            std::cout << "MH3 runtime self-test passed\n";
            return 0;
        }
        InitializeDataSections();
        RecompMod::RegisterExecutableRange(0x80004000, 0x800066E0, ".init");
        RecompMod::RegisterExecutableRange(0x8003F260, 0x8056FAA0, ".text");
        // Standard Wii boot information, with MH3's own arena boundary.
        Memory::Write32(0x80000000, 0x524D4850); // RMHP
        Memory::Write16(0x80000004, 0x3038); // 08
        Memory::Write32(0x80000020, 0x0D15EA5E);
        Memory::Write32(0x80000028, 24 * 1024 * 1024);
        Memory::Write32(0x8000002C, 0x23);
        Memory::Write32(0x80000030, 0x807AFCB8);
        Memory::Write32(0x80000034, 0x81800000);
        Memory::Write32(0x800000CC, 1); // PAL
        Memory::Write32(0x800000F8, 243000000);
        Memory::Write32(0x800000FC, 729000000);
        cpu.gpr[1] = 0x807AFCB8;
        cpu.msr = 0x8000;
        TranslatedFunctionRegistry::Finalize();
        std::cout << "Starting statically recompiled MH3 at 0x80006310\n";
        InvokeIndirectCpu(0x80006310, &cpu);
        std::cout << "MH3 entry returned\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MH3 runtime stopped: " << error.what() << '\n';
        SystemBridge::DumpCpuState(&GetPersistentCpuContext());
        return 1;
    }
}
