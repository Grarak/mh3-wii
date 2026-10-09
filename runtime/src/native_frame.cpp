// RMHP08 frame boundaries, reconstructed from FrameSetup/DisplayCopy.
// Native rendering replaces viewport setup, cache invalidation, EFB display
// copying and GXDrawDone. The backend submits/presents without a GPU wait.
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"
#include "native_graphics.h"
#include "runtime/native/adapters/model_pose_bridge.h"
#include <stdexcept>

// GXInit only created the FIFO and initial GPU state, then cleared/copied both
// old XFBs. Aurora owns the actual display surfaces; native frame begin sets
// viewport and clears the color/depth target once admission succeeds.
extern "C" void MH3_NativeGraphicsInit(CpuContext* cpu) {
    const auto mode = Memory::Read32(cpu->gpr[13] - 0x46B8);

    if (!mode || !Memory::Contains(mode, 60))
        throw std::runtime_error("Native graphics startup has no render mode");

    InitializeNativeGraphics(Memory::Read16(mode + 4), Memory::Read16(mode + 6));

    // Verified __GXInitRevisionBits enables XF DUALTEX in the original init.
    PublishNativeModelDualTexture(true);
}
REGISTER_NATIVE_FUNCTION_AS(0x8003FD30, MH3_NativeGraphicsInit, "NativeGraphicsInit");

// Preserve the boot VI configuration, compatibility XFB bookkeeping, and
// initial retraces used by the game's timing. These calls update the emulated
// VI; they do not copy EFBs, submit GXM work, or wait for GPU results.
extern "C" void MH3_NativeDisplaySetup(CpuContext* cpu) {
    GuestFrame frame(cpu, 0x10);
    GuestCall call{cpu};
    const auto sda = frame.Arg(13);
    const auto mode = Memory::Read32(sda - 0x46B8);

    if (!mode || !Memory::Contains(mode, 60))
        throw std::runtime_error("Native display setup has no render mode");

    const auto index = Memory::Read32(sda - 0x46A4);

    if (index > 1)
        throw std::runtime_error("Boot VI framebuffer index exceeds its compatibility pair");

    call(0x804E85E0, {mode});
    call(0x804E9130, {Memory::Read32(sda - 0x46B0 + index * 4)});
    Memory::Write32(sda - 0x46A4, index ^ 1);
    Memory::Write32(sda - 0x46A8, Memory::Read32(sda - 0x46B0 + (index ^ 1) * 4));
    call(0x804E9010);
    call(0x804E8050);

    if (Memory::Read32(mode) & 1)
        call(0x804E8050);
}
REGISTER_NATIVE_FUNCTION_AS(0x8003FE94, MH3_NativeDisplaySetup, "NativeDisplaySetup");

// 8003fe88 installed 8003fe84 as the GX draw-done callback. Decompilation of
// that four-byte callback confirms it returns without any application work.
// Native buffer retirement uses Aurora's notifications, so no callback is
// installed. Other calls to the SDK callback setter still hit the GX tripwire.
extern "C" void MH3_NativeCompletionSetup(CpuContext*) {}
REGISTER_NATIVE_FUNCTION_AS(0x8003FE88, MH3_NativeCompletionSetup, "NativeCompletionSetup");

extern "C" void MH3_NativeFrameBegin(CpuContext*) {
    BeginNativeGraphicsFrame();
}
REGISTER_NATIVE_FUNCTION_AS(0x8003FF20, MH3_NativeFrameBegin, "NativeFrameBegin");

extern "C" void MH3_NativeFrameEnd(CpuContext*) {
    EndNativeGraphicsFrame();
}
REGISTER_NATIVE_FUNCTION_AS(0x8003FFFC, MH3_NativeFrameEnd, "NativeFrameEnd");
