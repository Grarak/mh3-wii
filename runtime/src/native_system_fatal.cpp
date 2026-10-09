// OSFatal is terminal. Preserve its CPU diagnostic publication and route
// colors/text through native crash reporting without Wii MMIO or GX startup.
#include "abi_bridge.h"
#include "memory.h"
#include "system_bridge.h"
#include "runtime/native/mh3_system_fatal.h"
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef __vita__
#include <psp2/kernel/processmgr.h>
#include <unwind.h>
#endif

namespace {
#ifdef __vita__
_Unwind_Reason_Code NativeFrame(_Unwind_Context *context,void *user) {
    auto& count=*static_cast<unsigned*>(user);
    const uintptr_t address=_Unwind_GetIP(context);
    if(!address || count==64) return _URC_END_OF_STACK;
    std::fprintf(stderr,"  #%u %p\n",count++,reinterpret_cast<void*>(address));
    return _URC_NO_REASON;
}
#endif
mh3_res_span FatalMessage(uint32_t address) {
    size_t bytes=0;
    do {
        const uint64_t cursor=uint64_t(address)+bytes;
        if(cursor>UINT32_MAX || !Memory::Contains(uint32_t(cursor),1))
            throw std::runtime_error("OSFatal message is not terminated in readable RAM");
        ++bytes;
    } while(Memory::Read8(address+uint32_t(bytes)-1));
    return {Memory::GetPointer(address,bytes),bytes};
}
}

extern "C" [[noreturn]] void MH3_OSFatal(CpuContext *cpu) {
    std::string diagnostic="MH3 OSFatal: unreadable fatal arguments";
    try {
        const uint32_t foreground=cpu->gpr[3],background=cpu->gpr[4],text=cpu->gpr[5];
        const mh3_res_span fg{Memory::GetPointer(foreground,4),4},bg{Memory::GetPointer(background,4),4};
        const auto message=FatalMessage(text);
        size_t required=0;
        if(mh3_system_fatal_format(fg,bg,message,{},&required)!=MH3_RES_RANGE || !required)
            throw std::runtime_error("OSFatal diagnostic sizing failed");
        std::vector<uint8_t> output(required);
        if(mh3_system_fatal_format(fg,bg,message,{output.data(),output.size()},&required)!=MH3_RES_OK)
            throw std::runtime_error("OSFatal diagnostic formatting failed");
        diagnostic.assign(reinterpret_cast<const char*>(output.data()),required-1);
        // Original804CFFC0..804CFFD8 publishes these before switching stack
        // to the terminal fatal-display function804D0010.
        Memory::Write32(0x8074FAE8,Memory::Read32(foreground));
        Memory::Write32(0x8074FAEC,Memory::Read32(background));
        Memory::Write32(0x8074FAF0,text);
    } catch(const std::exception& error) {
        diagnostic+=" (";diagnostic+=error.what();diagnostic+=")";
    }
    RuntimeCrash::WriteCrashArtifacts("os_fatal",diagnostic);
    ShowRuntimeFatalPopup("MH3 fatal error",diagnostic);
    MarkFatalErrorReported();
    DumpHostStackTraceForRuntimeHelper();
#ifdef __vita__
    unsigned frames=0;
    std::fputs("Native OSFatal backtrace:\n",stderr);
    _Unwind_Backtrace(NativeFrame,&frames);
    std::fflush(nullptr);
    sceKernelExitProcess(1);
#endif
    std::fflush(nullptr);
    std::_Exit(EXIT_FAILURE);
}
REGISTER_NATIVE_FUNCTION_AS(0x804CFE20,MH3_OSFatal,"OSFatal");
