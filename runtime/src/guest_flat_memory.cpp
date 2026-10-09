// The flat guest window (guest_flat_memory.h): one PROT_NONE reservation with
// MEM1, MEM2 and the locked cache mapped into it from a memfd, and a SIGSEGV/
// SIGBUS reporter that names the guest address and translated function of any
// access that lands on an unmapped part of it.
#include "guest_flat_memory.h"
#include "recomp_mod_loader.h"
#include "gx_panic.h"

#include <dlfcn.h>
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace GuestFlat {
namespace {

constexpr uint32_t kMem1Size = 24u << 20;
constexpr uint32_t kMem2Size = 64u << 20;
constexpr uint32_t kLockedCacheSize = 0x4000;

#if defined(MH3_GUEST_WINDOW_WRAPS)
// Guest 0x80000000 up to the end of the locked cache.
constexpr uint32_t kWindowGuestStart = 0x80000000u;
constexpr uint64_t kWindowSize = 0xE0004000ull - kWindowGuestStart;
#else
constexpr uint32_t kWindowGuestStart = 0;
constexpr uint64_t kWindowSize = 1ull << 32;
#endif

int memoryFd = -1;
bool reserved = false;
struct sigaction previousSegv{}, previousBus{};

size_t PageSize() {
    static const size_t size = size_t(sysconf(_SC_PAGESIZE));
    return size;
}

size_t RoundUp(size_t value) { return (value + PageSize() - 1) & ~(PageSize() - 1); }

const char* Describe(uint32_t address) {
    if (IsHardwareAddress(address)) {
        const uint32_t p = address & 0x0FFFFFFFu;
        if (p >= 0x08000000u && p < 0x09000000u) return "EFB";
        if ((p & 0xFFFFFF00u) == 0x0C008000u) return "GX FIFO";
        switch (p >> 12) {
        case 0x0C000: return "CP";
        case 0x0C001: return "PE";
        case 0x0C002: return "VI";
        case 0x0C003: return "PI";
        case 0x0C004: return "MI";
        case 0x0C005: return "DSP";
        case 0x0C006: case 0x0D006: return "DI/SI/EXI/AI";
        case 0x0D000: return "IPC/Hollywood";
        default: return "MMIO";
        }
    }
    if (address >> 30 == 2 || address >> 30 == 3) {
        const uint32_t p = address & 0x3FFFFFFFu;
        if ((p >= kMem1Size && p < 0x04000000u) || (p >= 0x10000000u + kMem2Size && p < 0x14000000u))
            return "past the end of MEM1/MEM2";
    }
    return "unmapped";
}

bool FaultWasWrite(const void* context) {
    const auto* uc = static_cast<const ucontext_t*>(context);
#if defined(__aarch64__)
    // The ESR record in the signal frame carries WnR (bit 6) for data aborts.
    for (auto* head = reinterpret_cast<const _aarch64_ctx*>(uc->uc_mcontext.__reserved); head->magic;
         head = reinterpret_cast<const _aarch64_ctx*>(reinterpret_cast<const uint8_t*>(head) + head->size)) {
        if (head->magic == ESR_MAGIC) return (reinterpret_cast<const esr_context*>(head)->esr & (1u << 6)) != 0;
    }
    return false;
#elif defined(__arm__)
    return (uc->uc_mcontext.error_code & (1u << 11)) != 0;
#elif defined(__x86_64__)
    return (uc->uc_mcontext.gregs[REG_ERR] & 2) != 0;
#else
    return false;
#endif
}

uintptr_t FaultPc(const void* context) {
    const auto* uc = static_cast<const ucontext_t*>(context);
#if defined(__aarch64__)
    return uc->uc_mcontext.pc;
#elif defined(__arm__)
    return uc->uc_mcontext.arm_pc;
#elif defined(__x86_64__)
    return uc->uc_mcontext.gregs[REG_RIP];
#else
    return 0;
#endif
}

void Report(int signal, siginfo_t* info, void* context) {
    const uintptr_t host = reinterpret_cast<uintptr_t>(info->si_addr);
    const uintptr_t windowStart = kGuestBase + kWindowGuestStart;
    if (host - windowStart >= kWindowSize) {
        // Not a guest access: the previous disposition handles it on the retry.
        sigaction(signal, signal == SIGBUS ? &previousBus : &previousSegv, nullptr);
        return;
    }
    const uint32_t guest = static_cast<uint32_t>(host - kGuestBase);
    const uintptr_t pc = FaultPc(context);
    Dl_info symbol{};
    const bool named = dladdr(reinterpret_cast<void*>(pc), &symbol) && symbol.dli_sname;
    char text[512];
    const int length = std::snprintf(text, sizeof(text),
        "MH3 guest memory fault: %s of guest 0x%08x (%s) by %s+0x%lx (host pc %p), "
        "translated function 0x%08x. Hardware accesses belong to a function listed in "
        "runtime/src/hardware_access.inc.\n",
        FaultWasWrite(context) ? "write" : "read", guest, Describe(guest),
        named ? symbol.dli_sname : "?", named ? (unsigned long)(pc - uintptr_t(symbol.dli_saddr)) : 0ul,
        reinterpret_cast<void*>(pc), RecompMod::g_currentTranslatedExecutionAddress);
    if (length > 0) (void)!write(STDERR_FILENO, text, size_t(length) < sizeof(text) ? size_t(length) : sizeof(text) - 1);
    // An unrecognized, computed hardware pointer may bypass the translator's
    // FIFO lowering. Preserve the faulting native PC above and report its GX
    // guest/native call chain too, before ending the process.
    const uint32_t physical = guest & 0x3FFFFFFF;
    if ((guest >> 30) != 1 &&
        ((physical >= 0x0C000000 && physical < 0x0C002000) ||
         (physical >= 0x0C00300C && physical < 0x0C003018) ||
         (physical & 0xFFFFFF00u) == 0x0C008000 ||
         (physical & 0x3FC00000u) == 0x08000000))
        PanicLegacyGx(FaultWasWrite(context) ? "GX direct memory write" : "GX direct memory read", guest);
    // Default action on the retry: the process dies with the fault.
    signal == SIGBUS ? sigaction(SIGBUS, &previousBus, nullptr) : sigaction(SIGSEGV, &previousSegv, nullptr);
}

void InstallFaultReporter() {
    struct sigaction action{};
    action.sa_sigaction = Report;
    action.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, &previousSegv);
    sigaction(SIGBUS, &action, &previousBus);
}

void Map(uint32_t guest, size_t size, off_t offset) {
    void* at = Host(guest);
    if (mmap(at, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, memoryFd, offset) != at)
        throw std::runtime_error("Cannot map guest memory at 0x" + std::to_string(guest) + ": " + std::strerror(errno));
}

} // namespace

void Initialize() {
    if (!reserved) {
        void* start = Host(kWindowGuestStart);
        void* got = mmap(start, kWindowSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
        if (got != start) {
            if (got != MAP_FAILED) munmap(got, kWindowSize);
            char text[160];
            std::snprintf(text, sizeof(text), "Cannot reserve the guest window at host %p (+0x%llx): %s", start,
                          (unsigned long long)kWindowSize, got == MAP_FAILED ? std::strerror(errno) : "address taken");
            throw std::runtime_error(text);
        }
        reserved = true;
        InstallFaultReporter();
    }
    // A fresh object each time, so a re-initialization starts from zeroed memory.
    const int fd = memfd_create("mh3-guest-memory", MFD_CLOEXEC);
    if (fd < 0) throw std::runtime_error(std::string("memfd_create failed: ") + std::strerror(errno));
    const size_t lockedCache = RoundUp(kLockedCacheSize);
    if (ftruncate(fd, off_t(kMem1Size) + kMem2Size + lockedCache) != 0) {
        close(fd);
        throw std::runtime_error(std::string("Cannot size guest memory: ") + std::strerror(errno));
    }
    if (memoryFd >= 0) close(memoryFd);
    memoryFd = fd;
    for (uint32_t alias : {0x80000000u, 0xC0000000u}) {
        Map(alias, kMem1Size, 0);
        Map(alias + 0x10000000u, kMem2Size, kMem1Size);
    }
    Map(0xE0000000u, lockedCache, off_t(kMem1Size) + kMem2Size);
    g_mapped = true;
}


} // namespace GuestFlat
