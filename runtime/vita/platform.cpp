#include "platform.h"
#include "disc_image.h"
#include <aurora/thread_role.hpp>
#include <psp2/kernel/sysmem.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <psp2/appmgr.h>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>
#include <stdexcept>
#include <vector>

// All allocations in this heap are committed by newlib at startup. Guest RAM
// and GXM buffers have separate bounded memblocks; leave room for them within
// the retail main-memory budget rather than using the DevKit's extra memory.
extern "C" {
unsigned int _newlib_heap_size_user = 112 * 1024 * 1024;
unsigned int _pthread_stack_default_user = 1024 * 1024;
unsigned int _sceUserMainThreadStackSize = 2 * 1024 * 1024;
}

void InitializeVitaPlatform(int& argc, char**& argv) {
    aurora::pin_current_thread(aurora::ThreadRole::Game);
    // SceAppMgr supplies an empty argv[0], the SELF path and its own options.
    // Preserve the game's option/value pairs and give the shared parser a
    // conventional argv[0]. This also identifies debug launches from host0.
    static char program[] = "MH3";
    static std::vector<char*> arguments;
    arguments = {program};
    for (int i = 0; i < argc; ++i) {
        if (!argv[i] || !argv[i][0]) continue;
        const std::string_view value(argv[i]);
        if (value.ends_with("eboot.bin")) { arguments[0] = argv[i]; continue; }
        bool launchOption = false;
        for (const char* option : {"-budget", "-memsize", "-phcontmemsize", "-la_type", "-titleid", "-param", "-exit_to"}) {
            if (value == option) { launchOption = true; ++i; break; }
        }
        if (launchOption || value == "-livearea_off" || value == "-nonsuspendable" || value == "-usecdlg") continue;
        arguments.push_back(argv[i]);
    }
    argc = arguments.size();
    arguments.push_back(nullptr);
    argv = arguments.data();
    setenv("MH3_DISC_DIR", "ux0:/data/mh3-wii", 0);
    setenv("MH3_USER_DIR", "ux0:/data/mh3-wii/saves", 0);
#ifdef MH3_DEVELOPMENT
    std::fprintf(stderr, "MH3 Vita: main thread core 2, heap 112 MiB, argv0=%s\n", argc ? argv[0] : "");
    for (int i = 1; i < argc; ++i) std::fprintf(stderr, "MH3 Vita: argv[%d]=%s\n", i, argv[i]);
    SceKernelFreeMemorySizeInfo memory{};
    memory.size = sizeof(memory);
    if (sceKernelGetFreeMemorySize(&memory) >= 0)
        std::fprintf(stderr, "MH3 Vita: free main %.2f MiB, CDRAM %.2f MiB, phycont %.2f MiB before guest RAM\n",
            memory.size_user / 1048576.0, memory.size_cdram / 1048576.0, memory.size_phycont / 1048576.0);
#else
    (void)argc;
    (void)argv;
#endif
    // Match Azahar's documented native clock setup using official ScePower
    // rates. Query the resulting clocks in diagnostics rather than assuming
    // the DevKit's shell left them at the intended performance setting.
    const int armClock = scePowerSetArmClockFrequency(444);
    const int busClock = scePowerSetBusClockFrequency(222);
    const int gpuClock = scePowerSetGpuClockFrequency(222);
    const int xbarClock = scePowerSetGpuXbarClockFrequency(166);
    std::fprintf(stderr, "MH3 Vita clocks: CPU %d, bus %d, GPU %d, crossbar %d MHz (set %d/%d/%d/%d)\n",
        scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(), scePowerGetGpuClockFrequency(),
        scePowerGetGpuXbarClockFrequency(), armClock, busClock, gpuClock, xbarClock);
    ReportVitaMemory();
}

void ReportVitaMemory() {
    static uint64_t last = 0, peakMain = 0;
    const uint64_t now = sceKernelGetProcessTimeWide();
    if (last && now - last < 1000000) return;
    last = now;
    static bool budgetAvailable = true;
    if (budgetAvailable) {
        SceAppMgrBudgetInfo info{};
        info.size = sizeof(info);
        const int result = sceAppMgrGetBudgetInfo(&info);
        if (result >= 0 && info.total_user_rw_mem && info.free_user_rw <= info.total_user_rw_mem &&
            info.free_extra_mem <= info.total_extra_mem) {
            const uint64_t main = uint64_t(info.total_user_rw_mem) - info.free_user_rw +
                                  uint64_t(info.total_extra_mem) - info.free_extra_mem;
            peakMain = std::max(peakMain, main);
            std::fprintf(stderr, "MH3 Vita budget: main %.2f MiB, peak %.2f MiB; CDRAM %.2f MiB; phycont %.2f MiB\n",
                main / 1048576.0, peakMain / 1048576.0,
                (info.total_cdram_mem - info.free_cdram_mem) / 1048576.0,
                (info.total_phycont_mem - info.free_phycont_mem) / 1048576.0);
            if (main >= 365000000) throw std::runtime_error("Vita main RAM exceeds the 365 MB port limit");
            return;
        }
        budgetAvailable = false;
        std::fprintf(stderr, "MH3 Vita: per-app budget unavailable (0x%08x); reporting free partitions only\n", unsigned(result));
    }
    SceKernelFreeMemorySizeInfo memory{};
    memory.size = sizeof(memory);
    if (sceKernelGetFreeMemorySize(&memory) >= 0)
        std::fprintf(stderr, "MH3 Vita free: main %.2f MiB, CDRAM %.2f MiB, phycont %.2f MiB\n",
            memory.size_user / 1048576.0, memory.size_cdram / 1048576.0, memory.size_phycont / 1048576.0);
}

