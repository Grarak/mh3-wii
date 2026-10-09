#include "abi_bridge.h"
#include "gx_host.h"
#include "efb_host.h"

#ifndef __vita__
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <functional>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#ifdef MH3_HAS_AURORA
#include <dolphin/gx.h>
namespace aurora::gx::fifo { uint32_t process(const uint8_t*, uint32_t, bool); }
#endif

namespace {
void ExpectPanic(const char* name, const char* message, CpuContext* cpu,
                 const std::function<void()>& operation) {
    int pipeFd[2];
    if (pipe(pipeFd)) throw std::runtime_error("GX panic test pipe failed");
    std::fflush(nullptr);
    const pid_t child = fork();
    if (child < 0) {
        close(pipeFd[0]); close(pipeFd[1]);
        throw std::runtime_error("GX panic test fork failed");
    }
    if (!child) {
        close(pipeFd[0]);
        dup2(pipeFd[1], STDERR_FILENO);
        close(pipeFd[1]);
        // Construct a real PPC linkage chain; saved LR belongs to parent SP+4.
        cpu->lr = 0x8003F794;
        cpu->pc = 0x8003F7A0;
        cpu->gpr[1] = 0x817F0000;
        Memory::Write32(0x817F0000, 0x817F0020);
        Memory::Write32(0x817F0020, 0);
        Memory::Write32(0x817F0024, 0x8003FA48);
        operation();
        std::_Exit(99); // A disabled entry must never return to its caller.
    }
    close(pipeFd[1]);
    std::string output;
    bool timedOut = false;
    for (;;) {
        pollfd descriptor{pipeFd[0], POLLIN | POLLHUP, 0};
        const int ready = poll(&descriptor, 1, 5000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0 || output.size() > 65536) { timedOut = true; kill(child, SIGKILL); break; }
        char bytes[2048];
        const ssize_t count = read(pipeFd[0], bytes, sizeof(bytes));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        output.append(bytes, size_t(count));
    }
    close(pipeFd[0]);
    int status;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    if (timedOut || !WIFEXITED(status) || WEXITSTATUS(status) != 1 ||
        output.find("MH3 GX PANIC: ") == std::string::npos ||
        output.find(message) == std::string::npos ||
        output.find("LR 0x8003f794") == std::string::npos ||
        output.find("caller 0x8003fa48") == std::string::npos ||
        output.find("Native backtrace") == std::string::npos ||
        output.find("#0 ") == std::string::npos)
        throw std::runtime_error(std::string("GX panic test failed: ") + name + "\n" + output);
}
}
#endif

void RunGxPanicSelfTests(CpuContext* cpu) {
#ifndef __vita__
    unsigned checked = 0;
    const auto expect = [&](const char* name, const char* message, auto operation) {
        ExpectPanic(name, message, cpu, operation); ++checked;
    };
    expect("direct SDK", "GXInit", [&] { InvokeDirectCpu<0x804B3D10>(cpu); });
    expect("indirect SDK", "GXInvalidateTexAll", [&] { InvokeIndirectCpu(0x804B8FA0, cpu); });
    expect("unnamed SDK", "func_804B4AE0", [&] { InvokeIndirectCpu(0x804B4AE0, cpu); });
    expect("scalar FIFO", "GX FIFO write", [] { GX_HLE_FIFO_Write32(0x61000000); });
    expect("float FIFO", "GX FIFO write", [] { GX_HLE_FIFO_WriteFloat(1.f); });
    expect("coalesced FIFO", "GX FIFO write", [] { const uint8_t command[5]{}; GX_HLE_FIFO_WriteBurst(command, 5); });
    expect("open FIFO window", "GX FIFO write", [] {
        uint8_t buffer[32]{};
        gGxFifoWindow = {buffer, buffer + sizeof(buffer)};
        GX_HLE_FIFO_Write8(0x61);
    });
    expect("full FIFO writer", "GX FIFO full write", [] { const uint8_t byte = 0; GX_HLE_FIFO_WriteFull(&byte, 1); });
    expect("guest bytes", "GX FIFO write", [] { const uint8_t byte = 0; SubmitGuestGxBytes(&byte, 1); });
    expect("unlowered FIFO write", "GX direct memory write", [] { MemoryInline::FlatWriteRam32(0xCC008000, 0x61000000); });
    expect("unlowered CP read", "GX direct memory read", [] {
        volatile uint16_t value = MemoryInline::FlatLoad<uint16_t>(0xCC000020);
        (void)value;
    });
    // Valid and unsupported GX offsets all trap, on each physical/cached alias.
    for (const uint32_t alias : {0u, 0x80000000u, 0xC0000000u}) {
        for (const uint32_t offset : {0x0C000000u, 0x0C000020u, 0x0C001008u, 0x0C00300Cu}) {
            const uint32_t address = offset | alias;
            expect("GX MMIO read", "GX CP/PE/FIFO register read", [&] { (void)Memory::Read16(address); });
            expect("GX MMIO write", "GX CP/PE/FIFO register write", [&] { Memory::Write16(address, 0xFFFF); });
        }
    }
    expect("CPU EFB read", "GX CPU EFB color read", [] { uint32_t pixel; ReadEfbColor32(0xC8000000, pixel); });
    expect("CPU EFB out-of-bounds read", "GX CPU EFB color read", [] { uint32_t pixel; ReadEfbColor32(0xC8000A00, pixel); });
    expect("CPU EFB write", "GX CPU EFB color write", [] { WriteEfbColor32(0x88000000, 0xFFFFFFFF); });
#ifdef MH3_HAS_AURORA
    expect("host GX API", "GXSetCopyClear", [] { GXSetCopyClear({20, 40, 80, 255}, GX_MAX_Z24); });
    expect("FIFO decoder", "aurora::gx::fifo::process", [] { const uint8_t command = 0; aurora::gx::fifo::process(&command, 1, true); });
#endif
    std::fprintf(stderr, "MH3 GX tripwire self-test: %u fatal paths, guest and native backtraces matched\n", checked);
#else
    (void)cpu; // Fatal-path isolation uses Linux fork; the same entries ship on Vita.
#endif
}
