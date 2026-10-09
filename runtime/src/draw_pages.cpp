// MH3_TRACE=draw-pages (development, Linux hosts): the guest RAM a frame's
// draw touches against what the next frame's logic writes. A draw running
// on another core beside the next logic step would read those pages while
// they change: these are what it has to snapshot, defer or wait for.
//
// From in-quest frame kStart on, each draw window (DrawScene to MovieDraw)
// runs with MEM1 and MEM2 (cached views) inaccessible: the first access to a
// page, on any thread, records it as the draw's (read or write, the
// translated function, the thread) and opens it. A sampler thread closes all
// of RAM again every kResampleUs, so a page's later accesses record the last
// time (and function) the draw used it, to that resolution. The next logic
// step runs with RAM read-only, and the pages the draw wrote inaccessible:
// the first write to a page records its writer, the logic phase and the
// microseconds into the logic; the first read of a page the draw wrote
// records the reader likewise. At the next draw, the pages the logic wrote
// that the draw had touched are the pair's conflicts, the ones the draw wrote
// that the logic read its results the logic depends on. The game thread's
// guest stack is its own, not shared state, and is left out. After kPairs
// pairs the totals go to stderr (MH3 draw pages lines) and RAM is opened
// again.
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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace DrawPages {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kStart = 1500; // frame bodies before the first recorded draw
constexpr uint32_t kPairs = 30;
constexpr uint32_t kResampleUs = 250;

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
    uint8_t drawRead, drawWrite, drawThreads, logicWrite, logicRead;
    uint8_t logicPhase, readPhase;
    uint32_t drawFunction, drawLastFunction, logicFunction, readFunction;
    uint32_t drawLastUs, logicUs, readUs;
};

const size_t pageBytes = size_t(sysconf(_SC_PAGESIZE));
const size_t mem1Pages = Memory::kMem1Size / pageBytes, totalPages = mem1Pages + Memory::kMem2Size / pageBytes;
uint8_t* const mem1 = GuestFlat::Host(Memory::kMem1CachedBase);
uint8_t* const mem2 = GuestFlat::Host(Memory::kMem2CachedBase);
std::vector<Page> pages;
std::atomic<int> mode{Off};
std::atomic<uint8_t> phase{0};
Clock::time_point logicStart, drawStart;
std::mutex protection; // a mode change and its protection against the sampler's
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
std::map<uint32_t, Writer> lastReaders; // the last draw function on each conflict page
std::map<uint32_t, Writer> resultReaders; // logic functions reading what the draw wrote
uint64_t drawPages = 0, logicPages = 0, conflictPages = 0, otherThreadPages = 0, drawWrittenPages = 0, resultPages = 0;
uint32_t phaseConflicts[4]{};
std::vector<uint32_t> conflictLastUs; // over all pairs: the draw's last use of each conflict page

// Per pair: the draw's length, its last use of a conflict page, the logic's
// first conflicting write and first read of a draw result.
struct Pair {
    uint32_t drawUs, lastConflictUs, firstConflictUs, firstResultUs;
};

std::vector<Pair> pairList;
uint32_t drawUs = 0;

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

uint32_t Since(Clock::time_point start) {
    return uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
}

bool IsWrite(const void* context) {
#if defined(__aarch64__)
    const auto* uc = static_cast<const ucontext_t*>(context);

    for (auto* head = reinterpret_cast<const _aarch64_ctx*>(uc->uc_mcontext.__reserved); head->magic;
         head = reinterpret_cast<const _aarch64_ctx*>(reinterpret_cast<const uint8_t*>(head) + head->size))
        if (head->magic == ESR_MAGIC)
            return (reinterpret_cast<const esr_context*>(head)->esr & (1u << 6)) != 0;
    return false;
#elif defined(__arm__)
    // The data fault status register's WnR bit.
    return (static_cast<const ucontext_t*>(context)->uc_mcontext.error_code & (1u << 11)) != 0;
#else
    return (static_cast<const ucontext_t*>(context)->uc_mcontext.gregs[REG_ERR] & 2) != 0;
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

    const bool write = IsWrite(context);
    int open = PROT_READ | PROT_WRITE;

    if (now == Draw) {
        if (!page.drawRead && !page.drawWrite)
            page.drawFunction = function;

        page.drawRead |= !write;
        page.drawWrite |= write;
        page.drawThreads |= syscall(SYS_gettid) == gameThread ? Game : Other;
        page.drawLastFunction = function;
        page.drawLastUs = Since(drawStart);
    } else if (write) {
        page.logicWrite = 1;
        page.logicFunction = function;
        page.logicPhase = phase.load(std::memory_order_relaxed);
        page.logicUs = Since(logicStart);
    } else {
        // A page the draw wrote, read first: writes still fault.
        page.logicRead = 1;
        page.readFunction = function;
        page.readPhase = phase.load(std::memory_order_relaxed);
        page.readUs = Since(logicStart);
        open = PROT_READ;
    }

    mprotect(PageAddress(index), pageBytes, open);
}

void Protect(int protection) {
    mprotect(mem1, Memory::kMem1Size, protection);
    mprotect(mem2, Memory::kMem2Size, protection);
}

bool Stack(size_t index) {
    return index >= stackFirst && index <= stackLast;
}

// Closes RAM again every kResampleUs while a draw runs, for the last uses.
void Sampler() {
    while (!finished) {
        {
            std::lock_guard lock(protection);

            if (mode.load(std::memory_order_acquire) == Draw)
                Protect(PROT_NONE);
        }

        std::this_thread::sleep_for(std::chrono::microseconds(kResampleUs));
    }
}

void Install() {
    gameThread = syscall(SYS_gettid);
    pages.assign(totalPages, Page{});
    struct sigaction action {};
    action.sa_sigaction = Handler;
    action.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, &previous);
    std::thread(Sampler).detach();
    std::fprintf(stderr, "MH3 draw pages: recording %u draw/logic pairs from frame %u (%zu pages of %zu bytes)\n", kPairs, kStart, totalPages, pageBytes);
}

// The pair just finished: the draw's pages against the logic's writes.
void Tally() {
    Pair pair{drawUs, 0, UINT32_MAX, UINT32_MAX};

    for (size_t i = 0; i < totalPages; ++i) {
        const Page& page = pages[i];
        const bool drawn = page.drawRead || page.drawWrite;
        drawPages += drawn;
        logicPages += page.logicWrite;
        otherThreadPages += (page.drawThreads & Other) != 0;

        if (Stack(i))
            continue;

        drawWrittenPages += page.drawWrite;

        if (page.logicRead) {
            ++resultPages;
            auto& r = resultReaders[page.readFunction];
            ++r.pages;
            r.phase = page.readPhase;
            r.earliestUs = std::min(r.earliestUs, page.readUs);
            pair.firstResultUs = std::min(pair.firstResultUs, page.readUs);
        }

        if (!drawn || !page.logicWrite)
            continue;

        ++conflictPages;
        auto& w = writers[page.logicFunction];
        ++w.pages;
        w.phase = page.logicPhase;
        w.earliestUs = std::min(w.earliestUs, page.logicUs);
        ++readers[page.drawFunction];
        auto& last = lastReaders[page.drawLastFunction];
        ++last.pages;

        // earliestUs holds the latest use here.
        last.earliestUs = last.earliestUs == UINT32_MAX ? page.drawLastUs : std::max(last.earliestUs, page.drawLastUs);
        ++phaseConflicts[page.logicPhase & 3];
        conflictLastUs.push_back(page.drawLastUs);
        pair.firstConflictUs = std::min(pair.firstConflictUs, page.logicUs);
        pair.lastConflictUs = std::max(pair.lastConflictUs, page.drawLastUs);
    }

    pairList.push_back(pair);
}

void Report() {
    std::fprintf(stderr, "MH3 draw pages: %u pairs; per pair %.0f pages drawn (%.0f also by other threads), %.0f written by the next logic, %.0f conflicts\n", pairs,
                 double(drawPages) / pairs, double(otherThreadPages) / pairs, double(logicPages) / pairs, double(conflictPages) / pairs);
    std::fprintf(stderr, "MH3 draw pages: conflicts by logic phase: input %u scene %u tasks %u timers %u\n", phaseConflicts[0], phaseConflicts[1], phaseConflicts[2], phaseConflicts[3]);
    std::fprintf(stderr, "MH3 draw pages: per pair %.0f pages written by the draw, %.0f of them read by the next logic\n", double(drawWrittenPages) / pairs, double(resultPages) / pairs);
    std::fprintf(stderr, "MH3 draw pages: per pair, us: draw length / its last use of a conflict page / first conflicting logic write / first logic read of a draw write\n");
    std::string line = "MH3 draw pages:";

    for (const Pair& pair : pairList) {
        char word[48];
        std::snprintf(word, sizeof word, " %u/%u/%u/%u", pair.drawUs, pair.lastConflictUs, pair.firstConflictUs == UINT32_MAX ? 0 : pair.firstConflictUs,
                      pair.firstResultUs == UINT32_MAX ? 0 : pair.firstResultUs);
        line += word;
    }

    std::fprintf(stderr, "%s\n", line.c_str());
    std::sort(conflictLastUs.begin(), conflictLastUs.end());

    if (!conflictLastUs.empty()) {
        const auto at = [](double q) { return conflictLastUs[size_t(q * double(conflictLastUs.size() - 1))]; };
        std::fprintf(stderr, "MH3 draw pages: the draw's last use of a conflict page, us into the draw: p10 %u p50 %u p90 %u max %u (resolution %u us)\n", at(0.1), at(0.5), at(0.9),
                     conflictLastUs.back(), kResampleUs);
    }

    std::vector<std::pair<uint32_t, Writer>> byWriter(writers.begin(), writers.end());
    std::sort(byWriter.begin(), byWriter.end(), [](const auto& a, const auto& b) { return a.second.pages > b.second.pages; });

    for (size_t i = 0; i < byWriter.size() && i < 40; ++i)
        std::fprintf(stderr, "MH3 draw pages: writer %08x %u pages, phase %u, earliest %u us\n", byWriter[i].first, byWriter[i].second.pages, byWriter[i].second.phase,
                     byWriter[i].second.earliestUs);

    std::vector<std::pair<uint32_t, uint32_t>> byReader(readers.begin(), readers.end());
    std::sort(byReader.begin(), byReader.end(), [](const auto& a, const auto& b) { return a.second > b.second; });

    for (size_t i = 0; i < byReader.size() && i < 40; ++i)
        std::fprintf(stderr, "MH3 draw pages: draw reader %08x %u pages\n", byReader[i].first, byReader[i].second);

    std::vector<std::pair<uint32_t, Writer>> byLast(lastReaders.begin(), lastReaders.end());
    std::sort(byLast.begin(), byLast.end(), [](const auto& a, const auto& b) { return a.second.earliestUs > b.second.earliestUs; });

    for (size_t i = 0; i < byLast.size() && i < 40; ++i)
        std::fprintf(stderr, "MH3 draw pages: last draw user %08x %u pages, latest %u us into the draw\n", byLast[i].first, byLast[i].second.pages, byLast[i].second.earliestUs);

    std::vector<std::pair<uint32_t, Writer>> byResult(resultReaders.begin(), resultReaders.end());
    std::sort(byResult.begin(), byResult.end(), [](const auto& a, const auto& b) { return a.second.pages > b.second.pages; });

    for (size_t i = 0; i < byResult.size() && i < 40; ++i)
        std::fprintf(stderr, "MH3 draw pages: logic reader of draw writes %08x %u pages, phase %u, earliest %u us\n", byResult[i].first, byResult[i].second.pages,
                     byResult[i].second.phase, byResult[i].second.earliestUs);
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
        {
            std::lock_guard lock(protection);
            mode.store(Off, std::memory_order_release);
            Protect(PROT_READ | PROT_WRITE);
        }

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
    std::lock_guard lock(protection);
    drawStart = Clock::now();
    mode.store(Draw, std::memory_order_release);
    Protect(PROT_NONE);
}

void DrawEnds() {
    if (!recording)
        return;

    // The next logic step: every page read-only, so its first write faults,
    // and the draw's own writes closed, so their first read does (the mode
    // first: an access on another thread meanwhile is recorded).
    std::lock_guard lock(protection);
    drawUs = Since(drawStart);
    mode.store(Logic, std::memory_order_release);
    Protect(PROT_READ);

    for (size_t i = 0; i < totalPages; ++i)
        if (pages[i].drawWrite && !Stack(i))
            mprotect(PageAddress(i), pageBytes, PROT_NONE);
}

} // namespace DrawPages
#endif
