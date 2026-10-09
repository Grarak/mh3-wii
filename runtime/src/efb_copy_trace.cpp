// Debug-only (MH3_ENABLE_EFB_COPY_TRACE): find out who reads GX texture-copy
// destinations in guest RAM. After each copy's eager readback, the pages it
// covers are write/read protected. The first host access faults; the handler
// records the host PC, the translated guest function and the delay since the
// copy, unprotects the region and lets the access retry. Reports go to stderr
// from the game thread, never from the signal handler.
//
// Kernel accesses (e.g. read(2) straight into a protected page) fail with
// EFAULT instead of faulting, so this build is not for normal play.
#include "efb_copy_trace.h"
#include "recomp_mod_loader.h"
#include "isa/ppc_isa_context.h"

#include <dlfcn.h>
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#if defined(__aarch64__)
#include <asm/sigcontext.h>
#endif
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <tuple>
#include <string>
#include <algorithm>

namespace EfbCopyTrace {
namespace {
using Clock = std::chrono::steady_clock;

struct Region {
    uintptr_t pageStart = 0, pageEnd = 0; // protected host pages
    uintptr_t start = 0, end = 0;         // exact copy bytes
    uint32_t guest = 0, width = 0, height = 0, format = 0;
    uint64_t frame = 0;
    Clock::time_point armed;
    std::atomic<bool> active{false};
};

struct Hit {
    uint32_t region;
    uintptr_t address, pc, lr;
    uint32_t translated, guestLr;
    bool write, inside, gameThread;
    int64_t ns;
    uint64_t frame;
};

constexpr size_t kRegions = 64, kHits = 4096;
Region regions[kRegions];
Hit hits[kHits];
std::atomic<uint32_t> hitCount{0};
uint32_t nextRegion = 0;
uint64_t frame = 0, copies = 0, touched = 0;
const long pageSize = sysconf(_SC_PAGESIZE);
struct sigaction previous {};
// The installing (game) thread; the handler runs on the faulting thread.
long gameThread = -1;

struct FaultContext {
    uintptr_t pc, lr;
    bool write;
};
FaultContext Decode(const ucontext_t* uc) {
#if defined(__aarch64__)
    // The ESR record in the signal frame carries WnR (bit 6) for data aborts.
    bool write = false;
    for (auto* head = reinterpret_cast<const _aarch64_ctx*>(uc->uc_mcontext.__reserved); head->magic;
         head = reinterpret_cast<const _aarch64_ctx*>(reinterpret_cast<const uint8_t*>(head) + head->size)) {
        if (head->magic == ESR_MAGIC) {
            write = (reinterpret_cast<const esr_context*>(head)->esr & (1u << 6)) != 0;
            break;
        }
    }
    return {uc->uc_mcontext.pc, uc->uc_mcontext.regs[30], write};
#else
    return {uc->uc_mcontext.arm_pc, uc->uc_mcontext.arm_lr, (uc->uc_mcontext.error_code & (1u << 11)) != 0};
#endif
}

void Handler(int signal, siginfo_t* info, void* context) {
    const auto address = reinterpret_cast<uintptr_t>(info->si_addr);
    for (uint32_t i = 0; i < kRegions; ++i) {
        auto& region = regions[i];
        if (!region.active.load(std::memory_order_acquire) || address < region.pageStart || address >= region.pageEnd)
            continue;
        mprotect(reinterpret_cast<void*>(region.pageStart), region.pageEnd - region.pageStart, PROT_READ | PROT_WRITE);
        region.active.store(false, std::memory_order_release);
        const auto* uc = static_cast<const ucontext_t*>(context);
        const uint32_t slot = hitCount.fetch_add(1, std::memory_order_relaxed);
        if (slot < kHits) {
            const auto* cpu = TryGetCpuContext();
            const auto fault = Decode(uc);
            hits[slot] = {i,
                          address,
                          fault.pc,
                          fault.lr,
                          RecompMod::CurrentTranslatedExecutionAddress(),
                          cpu ? cpu->lr : 0,
                          fault.write,
                          address >= region.start && address < region.end,
                          syscall(SYS_gettid) == gameThread,
                          std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - region.armed).count(),
                          region.frame};
        }
        return;
    }
    // Not ours: fall back to the previous disposition and let the access fault again.
    sigaction(signal, &previous, nullptr);
}

void Install() {
    static bool installed = false;
    if (installed)
        return;
    installed = true;
    gameThread = syscall(SYS_gettid);
    struct sigaction action {};
    action.sa_sigaction = Handler;
    action.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, &previous);
    std::fprintf(stderr, "MH3 EFB copy trace: armed (page size %ld)\n", pageSize);
}

std::string Describe(uintptr_t pc) {
    Dl_info info{};
    char text[256];
    if (dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_fname) {
        const char* file = std::strrchr(info.dli_fname, '/');
        std::snprintf(text, sizeof(text), "%s+0x%lx%s%s", file ? file + 1 : info.dli_fname, static_cast<unsigned long>(pc - reinterpret_cast<uintptr_t>(info.dli_fbase)), info.dli_sname ? " " : "",
                      info.dli_sname ? info.dli_sname : "");
    } else {
        std::snprintf(text, sizeof(text), "0x%lx", static_cast<unsigned long>(pc));
    }
    return text;
}

void Drain() {
    // Unique first accesses: (guest dest, host pc, write) -> count.
    static std::map<std::tuple<uint32_t, uintptr_t, bool>, uint64_t> seen;
    static uint32_t drained = 0;
    const uint32_t count = std::min<uint32_t>(hitCount.load(std::memory_order_acquire), kHits);
    for (; drained < count; ++drained) {
        const auto& hit = hits[drained];
        const auto& region = regions[hit.region];
        ++touched;
        auto& n = seen[{region.guest, hit.pc, hit.write}];
        if (n++ == 0) {
            std::fprintf(stderr,
                         "MH3 EFB copy trace: first %s of copy 0x%08x (%ux%u fmt %u) at +0x%lx%s, %.3f ms / %llu frames "
                         "after copy, %s thread\n    host pc %s\n    host lr %s\n    translated func 0x%08x, guest lr 0x%08x\n",
                         hit.write ? "WRITE" : "READ", region.guest, region.width, region.height, region.format, static_cast<unsigned long>(hit.address - region.start),
                         hit.inside ? "" : " (outside copy, same page)", hit.ns / 1e6, static_cast<unsigned long long>(frame - hit.frame), hit.gameThread ? "game" : "other", Describe(hit.pc).c_str(),
                         Describe(hit.lr).c_str(), hit.translated, hit.guestLr);
        }
    }
    if (hitCount.load() >= kHits) {
        // Ring exhausted; restart so long runs keep reporting.
        hitCount.store(0);
        drained = 0;
    }
}
} // namespace

void BeforeCopy(uintptr_t host, uint32_t bytes) {
    Install();
    // The readback writes these bytes; lift any protection that covers them.
    for (auto& region : regions) {
        if (!region.active.load() || host + bytes <= region.pageStart || host >= region.pageEnd)
            continue;
        mprotect(reinterpret_cast<void*>(region.pageStart), region.pageEnd - region.pageStart, PROT_READ | PROT_WRITE);
        region.active.store(false);
    }
}

void AfterCopy(uintptr_t host, uint32_t guest, uint32_t bytes, uint32_t width, uint32_t height, uint32_t format) {
    auto& region = regions[nextRegion];
    nextRegion = (nextRegion + 1) % kRegions;
    if (region.active.load()) {
        mprotect(reinterpret_cast<void*>(region.pageStart), region.pageEnd - region.pageStart, PROT_READ | PROT_WRITE);
        region.active.store(false);
    }
    const uintptr_t mask = ~static_cast<uintptr_t>(pageSize - 1);
    region.pageStart = host & mask;
    region.pageEnd = (host + bytes + pageSize - 1) & mask;
    region.start = host;
    region.end = host + bytes;
    region.guest = guest;
    region.width = width;
    region.height = height;
    region.format = format;
    region.frame = frame;
    region.armed = Clock::now();
    ++copies;
    region.active.store(true, std::memory_order_release);
    mprotect(reinterpret_cast<void*>(region.pageStart), region.pageEnd - region.pageStart, PROT_NONE);
}

namespace {
uint32_t suspended[kRegions];
uint32_t suspendedCount = 0;
} // namespace

void BeginHostRead(uintptr_t host, uint32_t bytes) {
    suspendedCount = 0;
    for (uint32_t i = 0; i < kRegions; ++i) {
        auto& region = regions[i];
        if (!region.active.load() || host + bytes <= region.pageStart || host >= region.pageEnd)
            continue;
        mprotect(reinterpret_cast<void*>(region.pageStart), region.pageEnd - region.pageStart, PROT_READ | PROT_WRITE);
        suspended[suspendedCount++] = i;
    }
}

void EndHostRead() {
    for (uint32_t k = 0; k < suspendedCount; ++k) {
        auto& region = regions[suspended[k]];
        if (region.active.load())
            mprotect(reinterpret_cast<void*>(region.pageStart), region.pageEnd - region.pageStart, PROT_NONE);
    }
    suspendedCount = 0;
}

void EndFrame() {
    ++frame;
    Drain();
    if (frame % 60 == 0) {
        std::fprintf(stderr, "MH3 EFB copy trace: %llu copies, %llu touched by the host so far\n", static_cast<unsigned long long>(copies), static_cast<unsigned long long>(touched));
    }
}
} // namespace EfbCopyTrace
