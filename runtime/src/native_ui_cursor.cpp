// Thin ABI for the UI root's depth clear/draw/cursor utility. Ordinary C owns
// draw ordering, eligibility, raster publications and coordinate arithmetic.
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"
#include "native_graphics.h"
#include "runtime/native/mh3_ui_cursor.h"
#include <stdexcept>
#include <string>

namespace {
void RasterRead(void *, mh3_raster_state *out) {
    *out = GetNativeDrawState().raster;
}
void RasterPublish(void *, const mh3_raster_state *raster) {
    SetNativeRaster(*raster);
}
void Clear(void *) {
    ClearNativeDepthTarget();
}
uint32_t Field(void *user, unsigned field) {
    auto &call = *static_cast<GuestCall *>(user);
    constexpr int32_t offsets[] = {-16588, -16516, -16520, -16556, -16552, -21004, -21000, -20996, -20984};
    if (field == MH3_CURSOR_WIDESCREEN)
        return Memory::Read8(0x8065B4D5);
    const uint32_t address = call.cpu->gpr[13] + uint32_t(offsets[field]);
    return field == MH3_CURSOR_VERTICAL ? Memory::Read16(address) : Memory::Read32(address);
}
uint32_t Virtual(void *user, uint32_t object, uint32_t slot) {
    return static_cast<GuestCall *>(user)->Virtual(object, slot);
}
mh3_res_status Position(void *, uint32_t mode, uint32_t row, uint32_t column, mh3_res_span *out) {
    // The original mulli/slwi/add sequence wraps at 32 bits, including signed
    // row/column values. Validate the resulting guest address, not the indices.
    const uint32_t address = (mode == 1 ? UINT32_C(0x80572658) : UINT32_C(0x805727F0)) + row * (mode == 1 ? 24u : 16u) + column * 4u;
    if (!Memory::Contains(address, 4))
        return MH3_RES_RANGE;
    *out = {Memory::GetPointer(address, 4), 4};
    return MH3_RES_OK;
}
void Cursor(void *user, int32_t x, int32_t y) {
    (*static_cast<GuestCall *>(user))(0x802E0FDC, {uint32_t(x), uint32_t(y), 32, 32, uint32_t(-16), 0});
}
} // namespace
extern "C" void MH3_UICursorDraw(CpuContext *cpu) {
    GuestFrame frame(cpu, 0x10);
    GuestCall call{cpu};
    const mh3_ui_cursor_callbacks callbacks{&call, RasterRead, RasterPublish, Clear, Field, Virtual, Position, Cursor};
    const auto status = mh3_ui_cursor_draw(&callbacks);
    if (status != MH3_RES_OK)
        throw std::runtime_error("Native UI cursor input failed: " + std::to_string(status));
}
REGISTER_NATIVE_FUNCTION_AS(0x80449800, MH3_UICursorDraw, "UIDrawAndCursor");
