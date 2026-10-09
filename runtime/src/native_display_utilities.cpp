// Thin application display ABI boundaries. State uses the native renderer;
// the original SDK entry points remain fatal guards.
#include "abi_bridge.h"
#include "native_graphics.h"

extern "C" void MH3_HomeButtonInitializeRendering(CpuContext*) {
    // Original8003FD28 publishes only GXSetColorUpdate(1).
    auto raster=GetNativeDrawState().raster;
    raster.color_write=1;
    SetNativeRaster(raster);
}
REGISTER_NATIVE_FUNCTION_AS(0x8003FD28, MH3_HomeButtonInitializeRendering, "hbm_InitGX__Fv");
