#include "platform.h"
#include <aurora/thread_role.hpp>
#include <psp2/kernel/sysmem.h>
#include <psp2/io/dirent.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>
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
}

