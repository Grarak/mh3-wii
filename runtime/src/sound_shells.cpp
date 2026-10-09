// The 3D sound manager's per-frame update (0x800D811C), RMHP08. C
// reconstruction from the disassembly. The manager (*(r13 - 17576),
// 0x80796E38) keeps the listeners (camera position +4 and direction +0x10,
// the second player's +0x1C/+0x28, the view matrices +0x29280/+0x292B0), 64
// sound sources (0xA48 bytes each from +0x34: u8 live +0, owner +4,
// kind +8, the owner's position hook +0xA44, the listeners copied to +0x18
// and +0x24) and the music slot +0x2961C. Each live source with an owner
// (a monster's source, kind 4, only while the monster is not in action
// (12, 255) when it has a hook) runs its hook, takes the listeners and is
// updated by kind: 1-3 0x800D854C (its 32 shell slots), 4 0x800D86DC, 5-7,
// 13, 14 0x800D8794, 9/11 and 10/12 0x800D8614 with 0 or 1. The guest calls
// stay guest calls; the port is the loop and the listener copies.
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"

#include <cstdint>

namespace {

uint32_t Read32(uint32_t a) {
    return MemoryInline::FlatRead32(a);
}

void Copy12(uint32_t dst, uint32_t src) {
    for (uint32_t k = 0; k < 12; k += 4)
        MemoryInline::FlatWrite32(dst + k, Read32(src + k));
}

constexpr uint32_t kManager = 0x80796E38u;   // r13 - 17576
constexpr uint32_t kOnlineMode = 0x8065AA84u; // u8, GetOnlineMode (0x800CF26C)
constexpr uint32_t kMyPlayer = 0x8065AA87u;   // u8, MyPlayerIndex (0x800CF3E8)

// The sources' position hooks' memo (GuestCall::CallAt).
const RawDispatchRecord* hookMemo = nullptr;

void Update(CpuContext* ctx) {
    GuestFrame frame(ctx, 0xF0);
    GuestCall game{ctx};
    const uint32_t manager = Read32(kManager);

    if (MemoryInline::FlatRead8(kOnlineMode) == 2)
        game(Read32(0x806ACB94u), {0x806ACB90u}); // the online session's hook (+4) on it

    MemoryInline::FlatWrite8(manager + 0x2927D, 0);
    uint32_t player = game.Call<0x800CFAF4>({2}); // get_move_work_adrs(2)

    if (player) {
        player += 0xB20 * uint32_t(int32_t(int8_t(MemoryInline::FlatRead8(kMyPlayer))));

        if (MemoryInline::FlatRead8(kOnlineMode) == 1)
            MemoryInline::FlatWrite8(manager + 0x2927D, uint8_t(game.Call<0x802EED70>({player})));
    }

    if (!manager)
        return;

    auto listener = [&](uint32_t pos, uint32_t dir, uint32_t view, uint32_t index) {
        game.Call<0x802BDD44>({frame[0x44]}); // get_camera_pos
        Copy12(pos, frame[0x44]);
        game.Call<0x802BDE78>({frame[0x38]}); // get_camera_direction
        Copy12(dir, frame[0x38]);
        game.Call<0x802BDCA4>({frame[0x80]}); // get_current_view_mtx
        game.Call<0x80053340>({view, frame[0x80]}); // MtxCopyWrapper
        game.Call<0x800E84FC>({view, index});
    };

    if ((game.Call<0x800CF27C>() & 0xFF) == 2) { // PlayMode_ck: split screen
        const uint32_t me = MemoryInline::FlatRead8(kMyPlayer);
        game.Call<0x800CF3F8>({0}); // SetMyPlayerIndex
        listener(manager + 4, manager + 0x10, manager + 0x29280, 0);
        game.Call<0x800CF3F8>({1});
        listener(manager + 0x1C, manager + 0x28, manager + 0x292B0, 1);
        game.Call<0x800CF3F8>({uint32_t(int32_t(int8_t(me)))});
    } else {
        game.Call<0x802BDD44>({frame[0x14]});
        Copy12(manager + 4, frame[0x14]);
        game.Call<0x802BDE78>({frame[8]});
        Copy12(manager + 0x10, frame[8]);
        Copy12(manager + 0x1C, manager + 4);
        Copy12(manager + 0x28, manager + 0x10);
        game.Call<0x802BDCA4>({frame[0x50]});
        game.Call<0x80053340>({manager + 0x29280, frame[0x50]});
        game.Call<0x800E84FC>({manager + 0x29280, 0});
        game.Call<0x800E84FC>({0, 1});

        if (player)
            Copy12(manager + 0x1C, player + 0x3C);
    }

    for (uint32_t i = 0; i < 64; ++i) {
        const uint32_t source = manager + 0x34 + i * 0xA48;
        const uint32_t owner = Read32(source + 4);

        if (!MemoryInline::FlatRead8(source) || !owner)
            continue;

        const uint32_t kind = Read32(source + 8);

        if (kind == 4 && Read32(source + 0xA44) && game.Call<0x8012D270>({owner, 12, 255}) == 1) // em_act_ck
            continue;

        if (const uint32_t hook = Read32(source + 0xA44))
            game.CallAt(hookMemo, hook, {Read32(source + 4), Read32(source + 8)});

        Copy12(source + 0x18, manager + 4);
        Copy12(source + 0x24, manager + 0x1C);

        switch (Read32(source + 8)) {
        case 1:
        case 2:
        case 3:
            game.Call<0x800D854C>({source});
            break;
        case 4:
            game.Call<0x800D86DC>({source});
            break;
        case 5:
        case 6:
        case 7:
        case 13:
        case 14:
            game.Call<0x800D8794>({source});
            break;
        case 9:
        case 11:
            game.Call<0x800D8614>({source, 0});
            break;
        case 10:
        case 12:
            game.Call<0x800D8614>({source, 1});
            break;
        default:
            break;
        }
    }

    const uint32_t music = manager + 0x2961C;

    if (MemoryInline::FlatRead8(music) == 1) {
        game.Call<0x800D9348>({0, music}); // ShellSlotUpdate
        game.Call<0x800D8468>({music});    // ShellSlotFinish
    }

    if (game.Call<0x8028F268>() == 0)
        game.Call<0x800DFE30>({manager, 0}); // MapSoundUpdate
}

} // namespace

extern "C" void func_800D811C(CpuContext* ctx);
extern "C" void MH3_SoundSourcesUpdate(CpuContext* ctx) {
    PortCheck::Call(ctx, "SoundSourcesUpdate", func_800D811C, Update);
}
REGISTER_NATIVE_FUNCTION_AS(0x800D811C, MH3_SoundSourcesUpdate, "SoundSourcesUpdate", kNativeKeepsNonvolatile);
