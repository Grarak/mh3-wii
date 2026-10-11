#include "platform.h"
#include "fatal.h"
#include "nand_recover.h"
#include <aurora/thread_role.hpp>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/appmgr.h>
#include <csignal>
#include <cstdio>
#include <unwind.h>
#include <algorithm>
#include <cstdlib>
#include <string>
#include <string_view>
#include <stdexcept>
#include <vector>

// All allocations in this heap are committed by newlib at startup. Guest RAM
// and GXM buffers have separate bounded memblocks; leave room for them within
// the retail main-memory budget (with 112 MiB a retail Vita still had 62 MiB of
// main RAM free in the field; the model converter's ~30 MiB workspace takes
// its own block of that from the first model conversion on).
extern "C" {
unsigned int _newlib_heap_size_user = 128 * 1024 * 1024;
unsigned int _pthread_stack_default_user = 1024 * 1024;
unsigned int _sceUserMainThreadStackSize = 2 * 1024 * 1024;
}

namespace {
_Unwind_Reason_Code PrintFrame(_Unwind_Context* context, void* count) {
    sceClibPrintf("MH3 abort:   pc 0x%08X\n", unsigned(_Unwind_GetIP(context)));
    return ++*static_cast<int*>(count) < 32 ? _URC_NO_REASON : _URC_END_OF_STACK;
}

// abort() on a thread that reports nothing itself (libstdc++ without
// exceptions aborts on a std::system_error with no message): the thread and
// its stack, through the kernel's TTY rather than the C library's streams.
// Then abort goes on (raise again, without this handler).
void ReportAbort(int) {
    SceKernelThreadInfo info{};
    info.size = sizeof(info);
    const SceUID thread = sceKernelGetThreadId();
    sceKernelGetThreadInfo(thread, &info);
    sceClibPrintf("MH3 abort on thread %s (0x%08X), stack 0x%08X+0x%X\n", info.name, unsigned(thread), unsigned(uintptr_t(info.stack)), unsigned(info.stackSize));
    int frames = 0;
    _Unwind_Backtrace(PrintFrame, &frames);
    std::signal(SIGABRT, SIG_DFL);
}
} // namespace

void InitializeVitaPlatform(int& argc, char**& argv) {
    aurora::pin_current_thread(aurora::ThreadRole::Game);
    std::signal(SIGABRT, ReportAbort);

    // SceAppMgr supplies an empty argv[0], the SELF path and its own options.
    // Preserve the game's option/value pairs and give the shared parser a
    // conventional argv[0].
    static char program[] = "MH3";
    static std::vector<char*> arguments;
    arguments = {program};

    for (int i = 0; i < argc; ++i) {
        if (!argv[i] || !argv[i][0])
            continue;

        const std::string_view value(argv[i]);

        if (value.ends_with("eboot.bin")) {
            arguments[0] = argv[i];
            continue;
        }

        bool launchOption = false;

        for (const char* option : {"-budget", "-memsize", "-phcontmemsize", "-la_type", "-titleid", "-param", "-exit_to"}) {
            if (value == option) {
                launchOption = true;
                ++i;
                break;
            }
        }

        if (launchOption || value == "-livearea_off" || value == "-nonsuspendable" || value == "-usecdlg")
            continue;

        arguments.push_back(argv[i]);
    }

    argc = arguments.size();
    arguments.push_back(nullptr);
    argv = arguments.data();
    setenv("MH3_DISC_DIR", "ux0:/data/mh3-wii", 0);
    setenv("MH3_USER_DIR", "ux0:/data/mh3-wii/saves", 0);
#ifdef MH3_DEVELOPMENT
    // The log goes to the Vita's TTY, whole lines.
    std::setvbuf(stderr, nullptr, _IOLBF, 1024);
    std::fprintf(stderr, "MH3 Vita: main thread core 2, heap 112 MiB, argv0=%s\n", argc ? argv[0] : "");

    for (int i = 1; i < argc; ++i)
        std::fprintf(stderr, "MH3 Vita: argv[%d]=%s\n", i, argv[i]);

    SceKernelFreeMemorySizeInfo memory{};
    memory.size = sizeof(memory);

    if (sceKernelGetFreeMemorySize(&memory) >= 0)
        std::fprintf(stderr, "MH3 Vita: free main %.2f MiB, CDRAM %.2f MiB, phycont %.2f MiB before guest RAM\n", memory.size_user / 1048576.0, memory.size_cdram / 1048576.0,
                     memory.size_phycont / 1048576.0);
#else
    (void)argc;
    (void)argv;
#endif
    // Before the launcher lists saves or a quest launch reads one.
    RecoverNandSaves();

    // The highest clocks ScePower offers.
    const int armClock = scePowerSetArmClockFrequency(444);
    const int busClock = scePowerSetBusClockFrequency(222);
    const int gpuClock = scePowerSetGpuClockFrequency(222);
    const int xbarClock = scePowerSetGpuXbarClockFrequency(166);
    std::fprintf(stderr, "MH3 Vita clocks: CPU %d, bus %d, GPU %d, crossbar %d MHz (set %d/%d/%d/%d)\n", scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(), scePowerGetGpuClockFrequency(),
                 scePowerGetGpuXbarClockFrequency(), armClock, busClock, gpuClock, xbarClock);
    ReportVitaMemory();
}

void ReportVitaMemory() {
    static uint64_t last = 0, peakMain = 0, peakCdram = 0, peakPhycont = 0;
    const uint64_t now = sceKernelGetProcessTimeWide();

    if (last && now - last < 1000000)
        return;

    last = now;
    static bool budgetAvailable = true;

    if (budgetAvailable) {
        SceAppMgrBudgetInfo info{};
        info.size = sizeof(info);
        const int result = sceAppMgrGetBudgetInfo(&info);

        if (result >= 0 && info.total_user_rw_mem && info.free_user_rw <= info.total_user_rw_mem && info.free_extra_mem <= info.total_extra_mem && info.free_cdram_mem <= info.total_cdram_mem &&
            info.free_phycont_mem <= info.total_phycont_mem) {
            const uint64_t main = uint64_t(info.total_user_rw_mem) - info.free_user_rw + uint64_t(info.total_extra_mem) - info.free_extra_mem;
            peakMain = std::max(peakMain, main);
            const uint64_t cdram = uint64_t(info.total_cdram_mem) - info.free_cdram_mem;
            const uint64_t phycont = uint64_t(info.total_phycont_mem) - info.free_phycont_mem;
            peakCdram = std::max(peakCdram, cdram);
            peakPhycont = std::max(peakPhycont, phycont);
            std::fprintf(stderr, "MH3 Vita budget: main %.2f MiB, sampled peak %.2f MiB; CDRAM %.2f MiB, sampled peak %.2f MiB; phycont %.2f MiB, sampled peak %.2f MiB\n", main / 1048576.0,
                         peakMain / 1048576.0, cdram / 1048576.0, peakCdram / 1048576.0, phycont / 1048576.0, peakPhycont / 1048576.0);
            if (main >= 365000000)
                MH3_THROW(std::runtime_error("Vita main RAM exceeds the 365 MB port limit"));

            return;
        }

        budgetAvailable = false;
        std::fprintf(stderr, "MH3 Vita: per-app budget unavailable (0x%08x); reporting free partitions only\n", unsigned(result));
    }

    SceKernelFreeMemorySizeInfo memory{};
    memory.size = sizeof(memory);

    if (sceKernelGetFreeMemorySize(&memory) >= 0)
        std::fprintf(stderr, "MH3 Vita free: main %.2f MiB, CDRAM %.2f MiB, phycont %.2f MiB\n", memory.size_user / 1048576.0, memory.size_cdram / 1048576.0, memory.size_phycont / 1048576.0);
}
