// The field's gathering and carving spots, RMHP08. C reconstructions from the
// disassembly of the spot manager (the object at *(r13 - 16840), 0x80797118):
//
//   0x803BA604 GatherSpotsUpdate(): state 2 (just set up) clears the work
//              area (+0x34, 0x34800 bytes), builds the lists (0x803BE61C,
//              0x803BE6A4, 0x803BE750) and goes to 3; state 3 runs each live
//              entry's update (+0x1BC) of both lists
//   0x803BAA14 GatherSpotFind(player, playerIndex, pos, &kind, &spotPos,
//              &spotDir, &extra): the spot the hunter at `pos` can use, its
//              item table (0xFFFF for none)
//
// The manager: u8 state +0, the spots at +0x1C (count +0x20, 0x224 bytes
// each), the second list at +0x24 (count +0x28, 0x20C bytes each), the local
// player's position history +0x2C/+0x30. A spot (0x224 bytes): u8 live +0,
// u8 kind +3 (2: carving), u8 owner +4, s8 mode +6 (1: needs its object
// +0x1B8), its position +0x1D0, direction +0x1DC, extra word +0x1EC, radius
// +0x1F0, height +0x1F4, u16 item table +0x1F8, u16 uses left +0x1FA, s16
// item kind +0x1FC (-1 none, 0x11 a fish spot: u8 +0x21E is the result), its
// update +0x1BC.
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"
#include "ps_vec_math.h"

#include <cstdint>

namespace {

constexpr uint32_t kManager = 0x80797118u;   // r13 - 16840
constexpr uint32_t kMyPlayer = 0x8065AA87u;  // u8, what MyPlayerIndex (0x800CF3E8) returns
constexpr uint32_t kPlayerPos = 0x8065AA6Cu; // the local player's position word
constexpr uint32_t kHeightScale = 0x8079EA9Cu; // r2 - 5316

uint32_t Read32(uint32_t a) {
    return MemoryInline::FlatRead32(a);
}

void Write32(uint32_t a, uint32_t v) {
    MemoryInline::FlatWrite32(a, v);
}

float ReadFloat(uint32_t a) {
    return MemoryInline::FlatReadFloat32(a);
}

// VecMagWrapper (0x80050F88) of (dx, 0, dz), the planar distance (0x80050FE4).
float PlanarDistance(uint32_t pos, uint32_t spot) {
    const float dx = ReadFloat(spot) - ReadFloat(pos), dz = ReadFloat(spot + 8) - ReadFloat(pos + 8);
    return PsVec::Mag(dx, ReadFloat(0x8079809Cu), dz); // y: 0.0f (r2 - 32452)
}

uint32_t Find(uint32_t player, uint32_t index, uint32_t pos, uint32_t kindOut, uint32_t posOut, uint32_t dirOut, uint32_t extraOut) {
    const uint32_t manager = Read32(kManager);

    if (!manager)
        return 0xFFFF;

    if ((player & 0xFF) == uint32_t(int32_t(int8_t(MemoryInline::FlatRead8(kMyPlayer))))) {
        Write32(manager + 0x30, Read32(manager + 0x2C));
        Write32(manager + 0x2C, Read32(kPlayerPos));
    }

    const float scale = ReadFloat(kHeightScale);
    const float y = ReadFloat(pos + 4);

    for (int32_t i = int32_t(Read32(manager + 0x20)) - 1; i >= 0; --i) {
        const uint32_t spot = Read32(manager + 0x1C) + uint32_t(i) * 0x224;

        if (!MemoryInline::FlatRead8(spot) || MemoryInline::FlatRead8(spot + 4) != (index & 0xFF) || MemoryInline::FlatRead16(spot + 0x1FC) == 0xFFFF)
            continue;

        if (int8_t(MemoryInline::FlatRead8(spot + 6)) == 1 && !Read32(spot + 0x1B8))
            continue;

        const uint32_t kind = MemoryInline::FlatRead8(spot + 3);
        const uint32_t usesLeft = MemoryInline::FlatRead16(spot + 0x1FA);

        if (kind != 2 && usesLeft == 0)
            continue;

        // Within the spot's height band and radius.
        const float half = ReadFloat(spot + 0x1F4) * scale, center = ReadFloat(spot + 0x1D4);

        if (!(y >= center - half) || !(y < center + half))
            continue;

        if (!(PlanarDistance(pos, spot + 0x1D0) <= ReadFloat(spot + 0x1F0)))
            continue;

        const uint32_t table = MemoryInline::FlatRead16(spot + 0x1F8);
        MemoryInline::FlatWrite16(kindOut, MemoryInline::FlatRead16(spot + 0x1FC));

        for (uint32_t k = 0; k < 12; k += 4) {
            Write32(posOut + k, Read32(spot + 0x1D0 + k));
            Write32(dirOut + k, Read32(spot + 0x1DC + k));
        }

        if (extraOut)
            Write32(extraOut, Read32(spot + 0x1EC));
        else if (MemoryInline::FlatRead16(spot + 0x1FC) == 0x11)
            return MemoryInline::FlatRead8(spot + 0x21E);

        // A used-up spot: 0xFFFF for a carving spot (kind 2), else 0.
        if (MemoryInline::FlatRead16(spot + 0x1FA) == 0)
            return kind == 2 ? 0xFFFFu : 0u;

        return table;
    }

    return 0xFFFF;
}

// Memos of the two lists' update calls (GuestCall::CallAt).
const RawDispatchRecord* spotMemo = nullptr;
const RawDispatchRecord* entryMemo = nullptr;

void Update(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const uint32_t manager = Read32(kManager);

    if (!manager)
        return;

    const uint32_t state = MemoryInline::FlatRead8(manager);

    if (state == 2) {
        MemoryInline::FlatWrite8(manager, 3);
        game.Call<0x80004350>({manager + 0x34, 0, 0x34800}); // memset
        game.Call<0x803BE61C>();
        game.Call<0x803BE6A4>();
        game.Call<0x803BE750>();
        return;
    }

    if (state != 3)
        return;

    for (int32_t i = 0; i < int32_t(Read32(manager + 0x20)); ++i) {
        const uint32_t spot = Read32(manager + 0x1C) + uint32_t(i) * 0x224;

        if (MemoryInline::FlatRead8(spot))
            game.CallAt(spotMemo, Read32(spot + 0x1BC), {spot});
    }

    for (int32_t i = 0; i < int32_t(Read32(manager + 0x28)); ++i) {
        const uint32_t entry = Read32(manager + 0x24) + uint32_t(i) * 0x20C;

        if (MemoryInline::FlatRead8(entry))
            game.CallAt(entryMemo, Read32(entry + 0x1BC), {entry});
    }
}

} // namespace

// The body of GatherSpotFind, for the cheat wrapper registered over it
// (cheat_quest.cpp).
extern "C" void func_803BAA14(CpuContext* ctx);
static void GatherSpotFindPort(CpuContext* p) {
    p->gpr[3] = Find(p->gpr[3], p->gpr[4], p->gpr[5], p->gpr[6], p->gpr[7], p->gpr[8], p->gpr[9]);
}

void GatherSpotFind(CpuContext* ctx) {
    PortCheck::Call(ctx, "GatherSpotFind", func_803BAA14, GatherSpotFindPort);
}

extern "C" void func_803BA604(CpuContext* ctx);
extern "C" void MH3_GatherSpotsUpdate(CpuContext* ctx) {
    PortCheck::Call(ctx, "GatherSpotsUpdate", func_803BA604, Update);
}
REGISTER_NATIVE_FUNCTION_AS(0x803BA604, MH3_GatherSpotsUpdate, "GatherSpotsUpdate");
