// RMHP08 frame boundaries, reconstructed from FrameSetup/DisplayCopy.
// Native rendering replaces viewport setup, cache invalidation, EFB display
// copying and GXDrawDone. The backend submits/presents without a GPU wait.
#include "abi_bridge.h"
#include "native_graphics.h"

extern "C" void MH3_NativeFrameBegin(CpuContext*) {
    BeginNativeGraphicsFrame();
}
REGISTER_NATIVE_FUNCTION_AS(0x8003FF20, MH3_NativeFrameBegin, "NativeFrameBegin");

extern "C" void MH3_NativeFrameEnd(CpuContext*) {
    EndNativeGraphicsFrame();
}
REGISTER_NATIVE_FUNCTION_AS(0x8003FFFC, MH3_NativeFrameEnd, "NativeFrameEnd");
