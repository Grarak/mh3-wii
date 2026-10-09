// Development statistics for the overlapped draw (docs/pipelined-draw.md):
// how often logic steps change the scene graph the draw thread walks.
// ScnGroup insert (0x8008228C: group, index, object) and remove (0x80082438:
// group, index) shift a group's child array; the overlapped draw and
// MH3_PROFILE=draw-thread count them per caller (the MH3 scene changes line,
// each second with the frame split).
#include "abi_bridge.h"

#ifdef MH3_DEVELOPMENT
#include <cstdio>
#include <map>
#include <mutex>
#include <string>

namespace {
std::mutex lock;
std::map<std::pair<char, uint32_t>, uint32_t> calls; // (I or R, guest lr) -> calls

void Count(char kind, uint32_t lr) {
    std::lock_guard guard(lock);
    ++calls[{kind, lr}];
}
} // namespace

void MH3PrintSceneChanges(unsigned frames) {
    std::lock_guard guard(lock);

    if (calls.empty())
        return;

    std::string line = "MH3 scene changes (calls/frame by caller):";

    for (const auto& [key, count] : calls) {
        char part[48];
        std::snprintf(part, sizeof part, " %c@%08x %.1f", key.first, key.second, frames ? double(count) / frames : 0.0);
        line += part;
    }

    std::printf("%s\n", line.c_str());
    calls.clear();
}
#endif

extern "C" void func_8008228C(CpuContext* cpu);
extern "C" void MH3_ScnGroupInsert(CpuContext* cpu) {
#ifdef MH3_DEVELOPMENT
    Count('I', cpu->lr);
#endif
    func_8008228C(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8008228C, MH3_ScnGroupInsert, "ScnGroupInsert");

extern "C" void func_80082438(CpuContext* cpu);
extern "C" void MH3_ScnGroupRemove(CpuContext* cpu) {
#ifdef MH3_DEVELOPMENT
    Count('R', cpu->lr);
#endif
    func_80082438(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80082438, MH3_ScnGroupRemove, "ScnGroupRemove");
