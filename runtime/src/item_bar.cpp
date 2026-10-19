// The item bar, RMHP08, from the translated code:
//   0x8025E12C  ItemBarStep (PLW): the local player's (Pl_master_ck) bar each
//               step. While command bit 11 (L) is held it opens (+0x308, sound
//               5); bits 13 and 14 then scroll it: sound 6, the selected slot
//               (+0x304) moves to the next usable one forward or back
//               (ItemBarNextSlot) and +0x309 gets 2 or 4 for the HUD's turn.
//               Without bit 11 it closes.
//   0x8025DFDC  ItemBarNextSlot (PLW, slot, back) -> the next of the 26 pouch
//               slots (+0x278: u16 item, s16 count) after `slot` holding a
//               usable item, wrapping (back: before it)
// The captured mouse scrolls the open bar the same way: a left click back, a
// right click forward (controller_host.cpp takes the clicks while L is held).
#include "controller_host.h"
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"

#include <cstdint>

namespace {

enum : uint32_t {
    kSelectedSlot = 0x304, // u16 PLW
    kBarOpen = 0x308,      // u8 PLW
    kBarTurn = 0x309,      // u8 PLW: 2 forward, 4 back
};

enum : uint32_t {
    PlaySystemSound = 0x800DBC94, // (id)
    ItemBarNextSlot = 0x8025DFDC,
};

constexpr uint32_t kScrollSound = 6;

} // namespace

extern "C" void func_8025E12C(CpuContext* ctx);

extern "C" void MH3_ItemBarStep(CpuContext* ctx) {
    const uint32_t plw = ctx->gpr[3];
    func_8025E12C(ctx);
    const int scroll = TakeItemBarScroll();

    if (!scroll || !Memory::Read8(plw + kBarOpen))
        return;

    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const bool back = scroll < 0;

    for (int step = 0; step < (back ? -scroll : scroll); ++step) {
        game.Call<PlaySystemSound>({kScrollSound});
        const uint32_t slot = game.Call<ItemBarNextSlot>({plw, Memory::Read16(plw + kSelectedSlot), back ? 1u : 0u});
        Memory::Write16(plw + kSelectedSlot, uint16_t(slot));
        Memory::Write8(plw + kBarTurn, Memory::Read8(plw + kBarTurn) | (back ? 4 : 2));
    }
}

REGISTER_NATIVE_FUNCTION_AS(0x8025E12C, MH3_ItemBarStep, "ItemBarStep");
