// The small monster table's per-frame passes, RMHP08. C reconstructions from
// the disassembly. The table: 128 entries of 0x44 bytes at 0x806A7960 (u8
// live +0, u8 kind +2, u8 variant +3, u8 state +8 (1 and 2: not active),
// u8 icon slot +9 (0xFF none), u8 +0xA its mark):
//
//   0x801440E0 SmallMonstersUpdate(): each active entry's update
//              (0x80143EA0)
//   0x80343D48 MonsterIconsUpdate(): the map icons: for each large monster
//              (move work 3, 0xB18 bytes each) alive with an icon slot
//              (u8 +0x46C), then each active small monster with one, the
//              slot set (0x80343BD8) to its kind's icon (0x80343A38 of kind
//              +3 and variant +0xA) and mark (+0x1E1)
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"

#include <cstdint>

namespace {

constexpr uint32_t kTable = 0x806A7960u;
constexpr uint32_t kEntry = 0x44, kCount = 0x80;

bool Active(uint32_t entry) {
    const uint32_t state = MemoryInline::FlatRead8(entry + 8);
    return MemoryInline::FlatRead8(entry) && state != 1 && state != 2;
}

void Update(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x10);
    GuestCall game{ctx};

    for (uint32_t i = 0; i < kCount; ++i) {
        const uint32_t entry = kTable + i * kEntry;

        if (Active(entry))
            game.Call<0x80143EA0>({entry});
    }
}

void Icons(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    uint32_t em = game.Call<0x800CFAF4>({3}); // get_move_work_adrs
    const int32_t count = int32_t(game.Call<0x800CFB34>({3}) & 0xFFFF); // get_move_work_max

    for (int32_t i = 0; i < count; ++i, em += 0xB18) {
        if (game.Call<0x8012D068>({em}) == 1 || MemoryInline::FlatRead8(em + 0x46C) == 0xFF) // em_work_die_ck
            continue;

        const uint32_t icon = game.Call<0x80343A38>({MemoryInline::FlatRead8(em + 3), MemoryInline::FlatRead8(em + 0xA)});
        game.Call<0x80343BD8>({MemoryInline::FlatRead8(em + 0x46C), icon & 0xFF, MemoryInline::FlatRead8(em + 0x1E1)});
    }

    for (uint32_t i = 0; i < kCount; ++i) {
        const uint32_t entry = kTable + i * kEntry;

        if (!Active(entry) || MemoryInline::FlatRead8(entry + 9) == 0xFF)
            continue;

        const uint32_t icon = game.Call<0x80343A38>({MemoryInline::FlatRead8(entry + 2), MemoryInline::FlatRead8(entry + 3)});
        game.Call<0x80343BD8>({MemoryInline::FlatRead8(entry + 9), icon & 0xFF, MemoryInline::FlatRead8(entry + 0xA)});
    }
}

} // namespace

extern "C" void func_801440E0(CpuContext* ctx);
extern "C" void MH3_SmallMonstersUpdate(CpuContext* ctx) {
    PortCheck::Call(ctx, "SmallMonstersUpdate", func_801440E0, Update);
}
REGISTER_NATIVE_FUNCTION_AS(0x801440E0, MH3_SmallMonstersUpdate, "SmallMonstersUpdate");

extern "C" void func_80343D48(CpuContext* ctx);
extern "C" void MH3_MonsterIconsUpdate(CpuContext* ctx) {
    PortCheck::Call(ctx, "MonsterIconsUpdate", func_80343D48, Icons);
}
REGISTER_NATIVE_FUNCTION_AS(0x80343D48, MH3_MonsterIconsUpdate, "MonsterIconsUpdate");
