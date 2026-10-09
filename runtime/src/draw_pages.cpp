// MH3_TRACE=draw-pages (development, Linux hosts): the guest RAM a frame's
// draw touches against what the next frame's logic writes. A draw running
// on another core beside the next logic step would read those pages while
// they change: these are what it has to snapshot, defer or wait for.
//
// From in-quest frame kStart on, each draw window (DrawScene to MovieDraw)
// runs with MEM1 and MEM2 (cached views) inaccessible: the first access to a
// page, on any thread, records it as the draw's (read or write, the
// translated function, the thread) and opens it. The next logic step runs
// with RAM read-only: the first write to a page records its writer, the
// logic phase and the microseconds into the logic. At the next draw, the
// pages the logic wrote that the draw had touched are the pair's conflicts.
// The game thread's guest stack is its own, not shared state, and is left
// out. After kPairs pairs the totals go to stderr (MH3 draw pages lines) and
// RAM is opened again.
//
// Kernel accesses into a protected page (read(2) into guest RAM) fail with
// EFAULT instead of faulting; the disc reads in place through memcpy.
#include "draw_pages.h"

#if defined(MH3_DEVELOPMENT) && !defined(__vita__)
#include "dev_options.h"
#include "guest_flat_memory.h"
#include "memory.h"
#include "recomp_mod_loader.h"

#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#if defined(__aarch64__)
#include <asm/sigcontext.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace DrawPages {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kStart = 1500; // frame bodies before the first recorded draw
constexpr uint32_t kPairs = 30;

enum Mode : int {
    Off,
    Draw,
    Logic,
};

enum Thread : uint8_t {
    Game = 1,
    Other = 2,
};

struct Page {
    uint8_t drawRead, drawWrite, drawThreads, logicWrite;
    uint32_t drawFunction, logicFunction;
    uint8_t logicPhase;
    uint32_t logicUs;
};

const size_t pageBytes = size_t(sysconf(_SC_PAGESIZE));
const size_t mem1Pages = Memory::kMem1Size / pageBytes, totalPages = mem1Pages + Memory::kMem2Size / pageBytes;
uint8_t* const mem1 = GuestFlat::Host(Memory::kMem1CachedBase);
uint8_t* const mem2 = GuestFlat::Host(Memory::kMem2CachedBase);
std::vector<Page> pages;
std::atomic<int> mode{Off};
std::atomic<uint8_t> phase{0};
Clock::time_point logicStart;
long gameThread = -1;
struct sigaction previous {};

uint32_t frames = 0, pairs = 0;
size_t stackFirst = 0, stackLast = 0;
bool recording = false, finished = false;

// Totals over the pairs.
struct Writer {
    uint32_t pages = 0, earliestUs = UINT32_MAX;
    uint8_t phase = 0;
};

std::map<uint32_t, Writer> writers;
std::map<uint32_t, uint32_t> readers;
uint64_t drawPages = 0, logicPages = 0, conflictPages = 0, otherThreadPages = 0;
uint32_t phaseConflicts[4]{};
std::vector<uint32_t> firstConflictUs;

// The page index of a host address, or SIZE_MAX outside MEM1/MEM2.
size_t Index(uintptr_t address) {
    const auto a = reinterpret_cast<uintptr_t>(mem1), b = reinterpret_cast<uintptr_t>(mem2);

    if (address >= a && address < a + Memory::kMem1Size)
        return (address - a) / pageBytes;

    if (address >= b && address < b + Memory::kMem2Size)
        return mem1Pages + (address - b) / pageBytes;

    return SIZE_MAX;
}

uint8_t* PageAddress(size_t index) {
    return index < mem1Pages ? mem1 + index * pageBytes : mem2 + (index - mem1Pages) * pageBytes;
}

bool IsWrite(const void* context) {
#if defined(__aarch64__)
    const auto* uc = static_cast<const ucontext_t*>(context);

    for (auto* head = reinterpret_cast<const _aarch64_ctx*>(uc->uc_mcontext.__reserved); head->magic;
         head = reinterpret_cast<const _aarch64_ctx*>(reinterpret_cast<const uint8_t*>(head) + head->size))
        if (head->magic == ESR_MAGIC)
            return (reinterpret_cast<const esr_context*>(head)->esr & (1u << 6)) != 0;
    return false;
#else
    return (static_cast<const ucontext_t*>(context)->uc_mcontext.error_code & (1u << 11)) != 0;
#endif
}

void Handler(int signal, siginfo_t* info, void* context) {
    const size_t index = Index(reinterpret_cast<uintptr_t>(info->si_addr));
    const int now = mode.load(std::memory_order_acquire);

    if (index == SIZE_MAX || now == Off) {
        // Not ours: the previous handler (the guest fault reporter) decides.
        if (previous.sa_flags & SA_SIGINFO)
            previous.sa_sigaction(signal, info, context);
        else {
            sigaction(signal, &previous, nullptr);
        }

        return;
    }

    Page& page = pages[index];
    const uint32_t function = RecompMod::CurrentTranslatedExecutionAddress();

    if (now == Draw) {
        const bool write = IsWrite(context);
        page.drawRead |= !write;
        page.drawWrite |= write;
        page.drawThreads |= syscall(SYS_gettid) == gameThread ? Game : Other;

        if (!page.drawFunction)
            page.drawFunction = function;
    } else {
        page.logicWrite = 1;
        page.logicFunction = function;
        page.logicPhase = phase.load(std::memory_order_relaxed);
        page.logicUs = uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - logicStart).count());
    }

    mprotect(PageAddress(index), pageBytes, PROT_READ | PROT_WRITE);
}

void Protect(int protection) {
    mprotect(mem1, Memory::kMem1Size, protection);
    mprotect(mem2, Memory::kMem2Size, protection);
}

void Install() {
    gameThread = syscall(SYS_gettid);
    pages.assign(totalPages, Page{});
    struct sigaction action {};
    action.sa_sigaction = Handler;
    action.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, &previous);
    std::fprintf(stderr, "MH3 draw pages: recording %u draw/logic pairs from frame %u (%zu pages of %zu bytes)\n", kPairs, kStart, totalPages, pageBytes);
}

// The pair just finished: the draw's pages against the logic's writes.
void Tally() {
    uint32_t first = UINT32_MAX;

    for (size_t i = 0; i < totalPages; ++i) {
        const Page& page = pages[i];
        const bool drawn = page.drawRead || page.drawWrite;
        drawPages += drawn;
        logicPages += page.logicWrite;
        otherThreadPages += (page.drawThreads & Other) != 0;

        if (!drawn || !page.logicWrite || (i >= stackFirst && i <= stackLast))
            continue;

        ++conflictPages;
        auto& w = writers[page.logicFunction];
        ++w.pages;
        w.phase = page.logicPhase;
        w.earliestUs = std::min(w.earliestUs, page.logicUs);
        ++readers[page.drawFunction];
        ++phaseConflicts[page.logicPhase & 3];
        first = std::min(first, page.logicUs);
    }

    firstConflictUs.push_back(first);
}

void Report() {
    std::fprintf(stderr, "MH3 draw pages: %u pairs; per pair %.0f pages drawn (%.0f also by other threads), %.0f written by the next logic, %.0f conflicts\n", pairs,
                 double(drawPages) / pairs, double(otherThreadPages) / pairs, double(logicPages) / pairs, double(conflictPages) / pairs);
    std::fprintf(stderr, "MH3 draw pages: conflicts by logic phase: input %u scene %u tasks %u timers %u\n", phaseConflicts[0], phaseConflicts[1], phaseConflicts[2], phaseConflicts[3]);
    std::string line = "MH3 draw pages: first conflicting write, us into the logic:";

    for (const uint32_t us : firstConflictUs) {
        char word[16];
        std::snprintf(word, sizeof word, " %u", us == UINT32_MAX ? 0 : us);
        line += word;
    }

    std::fprintf(stderr, "%s\n", line.c_str());
    std::vector<std::pair<uint32_t, Writer>> byWriter(writers.begin(), writers.end());
    std::sort(byWriter.begin(), byWriter.end(), [](const auto& a, const auto& b) { return a.second.pages > b.second.pages; });

    for (size_t i = 0; i < byWriter.size() && i < 40; ++i)
        std::fprintf(stderr, "MH3 draw pages: writer %08x %u pages, phase %u, earliest %u us\n", byWriter[i].first, byWriter[i].second.pages, byWriter[i].second.phase,
                     byWriter[i].second.earliestUs);

    std::vector<std::pair<uint32_t, uint32_t>> byReader(readers.begin(), readers.end());
    std::sort(byReader.begin(), byReader.end(), [](const auto& a, const auto& b) { return a.second > b.second; });

    for (size_t i = 0; i < byReader.size() && i < 40; ++i)
        std::fprintf(stderr, "MH3 draw pages: draw reader %08x %u pages\n", byReader[i].first, byReader[i].second);
}

} // namespace

bool Enabled() {
    static const bool enabled = TraceOptions().Has("draw-pages");
    return enabled;
}

void LogicPhase(Phase next) {
    if (!recording)
        return;

    if (next == Phase::Input)
        logicStart = Clock::now();

    phase.store(uint8_t(next), std::memory_order_relaxed);
}

void DrawBegins(uint32_t stack) {
    if (finished || !Enabled() || ++frames < kStart)
        return;

    if (!recording) {
        Install();
        recording = true;
    } else {
        // The logic since the last draw is over: tally the pair.
        mode.store(Off, std::memory_order_release);
        Protect(PROT_READ | PROT_WRITE);
        Tally();

        if (++pairs == kPairs) {
            Report();
            recording = false;
            finished = true;
            sigaction(SIGSEGV, &previous, nullptr);
            return;
        }
    }

    const size_t stackPage = Index(reinterpret_cast<uintptr_t>(GuestFlat::Host(stack)));
    stackFirst = stackPage == SIZE_MAX ? 1 : stackPage - std::min<size_t>(stackPage, 0x20000 / pageBytes);
    stackLast = stackPage == SIZE_MAX ? 0 : stackPage + 0x4000 / pageBytes;
    std::fill(pages.begin(), pages.end(), Page{});
    mode.store(Draw, std::memory_order_release);
    Protect(PROT_NONE);
}

void DrawEnds() {
    if (!recording)
        return;

    // The next logic step: every page read-only, so its first write faults
    // (the mode first: a write on another thread meanwhile is recorded).
    mode.store(Logic, std::memory_order_release);
    Protect(PROT_READ);
}

} // namespace DrawPages
#endif
