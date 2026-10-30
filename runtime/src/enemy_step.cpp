// The large monsters' step, RMHP08. C reconstructions from the disassembly:
//
//   0x80143C78 MonstersMove(): the first of the field's two monster passes:
//              each large monster's MonsterStep (not in an event demo, unless
//              +0x46B holds a slot)
//   0x80143DDC MonstersActive(): after the hunters and the field's objects
//              moved, each monster's EnemyActiveStep and each hunter's
//              0x80144BC0
//   0x80138BC4 EnemyActiveStep(em): what only a monster in play does (u8
//              +0x1DE, set by MonsterStep): its handlers 6 and 8, its colours
//   0x80137434 MonsterColours(em): every material's channel 4 alpha and alpha
//              test from the resource, scaled by the fade (+0x1D4). Below 1
//              the material blends, at 1 its pixel copy is the resource's
//              again (0x800E3140 with 0x80), which undoes what a species'
//              handler 8 sets later in the step (enemy_parts.cpp)
//
// +0x764 and +0x768 count on whole ticks (tick_sites.inc had the first).
#include "abi_bridge.h"
#include "game_step.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"

#include <algorithm>
#include <cstdint>

namespace {

enum : uint32_t {
    kChar = 0x24,       // MHchar
    kFade = 0x1D4,      // f32
    kPlace = 0x1E2,     // u8
    kActive = 0x1DE,    // u8
    kFlags = 0x1C8,     // u32
    kMoved = 0x8C0,     // u8
    kOnce = 0x762,      // u8
    kCount = 0x764,     // s16
    kHeld = 0x766,      // u8
    kHolds = 0x767,     // u8
    kHoldCount = 0x768, // s16
};

enum : uint32_t {
    Handler = 0x80126520,
    MaterialCount = 0x800E2948,
    MaterialRefresh = 0x800E3140,
    SetWorld = 0x800E0978,
};

constexpr uint32_t kConnected = 0x806A6D7A, kConnectedEdge = 0x806A6D7B; // u8s: the online link, and the step it came up

uint32_t R8(uint32_t a) {
    return MemoryInline::FlatRead8(a);
}

int32_t S16(uint32_t a) {
    return int16_t(MemoryInline::FlatRead16(a));
}

void W8(uint32_t a, uint32_t v) {
    MemoryInline::FlatWrite8(a, uint8_t(v));
}

void W16(uint32_t a, int32_t v) {
    MemoryInline::FlatWrite16(a, uint16_t(v));
}

// The original's x - 1: by the step's whole ticks down to 0, then by 1.
int32_t CountDown(uint32_t a) {
    const int32_t x = S16(a);
    const int32_t value = x > 0 ? std::max(x - int32_t(GameStep::Ticks()), 0) : x - 1;
    W16(a, value);
    return value;
}

void Hold(GuestCall& game, uint32_t em) {
    if (R8(em + kHeld) != 0)
        return;

    if (R8(em + kHolds) == 1) {
        W8(em + kHeld, 1);
        game.Call<0x80041EA4>({em + 0x778, em + 0x76C});
    }

    if (S16(em + kHoldCount) > 0)
        CountDown(em + kHoldCount);
}

void Countdown764(GuestCall& game, uint32_t em) {
    if (S16(em + kCount) <= -1)
        return;

    if (game.Call<0x8012F368>({em, 0}) == 1) {
        W8(em + kOnce, 1);
        return;
    }

    const bool first = R8(em + kOnce) == 0;

    if (CountDown(em + kCount) >= 1)
        return;

    if (first) {
        W8(em + kOnce, 1);
        W16(em + kCount, 2);
    } else
        game.Call<0x80133BC0>({em});
}

void ActiveStep(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const uint32_t em = frame.Arg(3);

    if (R8(em + kActive) == 0)
        return;

    const bool moved = R8(em + kMoved) != 0;

    if (moved) {
        game.Call<0x80073FCC>({em + 0x188, em + 0x8B4});
        game.Call<0x801380D8>({em, 1});
        game.Call<0x8012A418>({em});
        game.Call<0x8012FF9C>({em});
    }

    int32_t spot = -1;

    if (R8(em + kPlace) == 1 && R8(em + 0x1E3) == 1) {
        spot = int32_t(game.Call<0x80137F44>({em, 0}));

        if (spot != -1)
            game.Call<0x80138088>({em, 0x400, uint32_t(spot)});
    }

    if (moved || spot != -1) {
        game.Call<0x80138EC8>({em});
        game.Call<SetWorld>({em + kChar});
    }

    game.Call<0x8012A478>({em});
    Hold(game, em);
    game.Call<0x80139260>({em});
    game.Call<0x8013645C>({em});
    Countdown764(game, em);

    if (const uint32_t handler = game.Call<Handler>({em, 6}))
        game(handler, {em});

    game.Call<0x80133444>({em});
    const uint32_t flags = MemoryInline::FlatRead32(em + kFlags);

    if (!(flags & 1))
        game.Call<0x80136E9C>({em, 0});
    else if (flags & 0x40000) {
        const bool dead = game.Call<0x8012D090>({em}) == 1;

        for (uint32_t i = 0; i < game.Call<MaterialCount>({em + kChar}); ++i) {
            if (dead)
                game.Call<MaterialRefresh>({em + kChar, i, 0x10});
            else
                game.Call<0x800E2F20>({em + kChar, i, 2});
        }
    }

    if (const uint32_t handler = game.Call<Handler>({em, 8}))
        game(handler, {em});

    game.Call<0x8012FCC4>({em});

    if (R8(em + kPlace) == 4)
        W16(em + 0x79C, 1);
}

void Colours(CpuContext* ctx) {
    GuestFrame frame(ctx, 0xB0);
    GuestCall game{ctx};
    const uint32_t em = frame.Arg(3), ch = em + kChar, pix = frame[56], colour = frame[24], copy = frame[12];
    game.Call<0x800767F8>({pix, 0});
    const uint32_t count = game.Call<MaterialCount>({ch});

    for (uint32_t i = 0; i < count; ++i) {
        MemoryInline::FlatWrite32(frame[16], game.Call<0x80098864>({ch + 0x114, i})); // the ResMdl's material
        MemoryInline::FlatWrite32(frame[20], game.Call<0x800767C8>({frame[16]}));
        game.Call<0x80078570>({pix, frame[20]});
        const float fade = MemoryInline::FlatReadFloat32(em + kFade);

        if (game.Call<0x800E2B58>({ch, i, 4, colour}) == 1) {
            W8(colour + 3, uint32_t(int32_t(255.0f * fade)));
            MemoryInline::FlatWrite32(copy, MemoryInline::FlatRead32(colour));
            game.Call<0x800E2950>({ch, i, 4, copy, 0});

            if (fade < 1.0f) {
                const uint32_t blend = frame[40], source = frame[36];
                game.Call<0x80096008>({pix, blend, source, frame[32], frame[28]});

                if (MemoryInline::FlatRead32(source) != 4 || MemoryInline::FlatRead32(blend) == 0)
                    game.Call<0x800E308C>({ch, i, 1, 4, 5, 0});
            } else
                game.Call<MaterialRefresh>({ch, i, 0x80});
        }

        const uint32_t compare0 = frame[52], reference0 = frame[9], operation = frame[44], compare1 = frame[48], reference1 = frame[8];
        game.Call<0x80095ECC>({pix, compare0, reference0, operation, compare1, reference1});
        const uint32_t ref0 = R8(reference0), ref1 = R8(reference1);

        if (ref0 != 0 || ref1 != 0xFF)
            game.Call<0x800E2FA4>({ch, i, MemoryInline::FlatRead32(compare0), uint32_t(int32_t(float(ref0) * fade)) & 0xFF, MemoryInline::FlatRead32(operation),
                                   MemoryInline::FlatRead32(compare1), uint32_t(int32_t(float(ref1) * fade)) & 0xFF});
    }
}

void MonstersMove(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    uint32_t monster = game.Call<0x800CFAF4>({3});
    const uint32_t monsters = game.Call<0x800CFB34>({3}) & 0xFFFF;

    if (game.Call<0x8045169C>() == 1) {
        const uint32_t on = game.Call<0x80451720>();
        W8(kConnectedEdge, R8(kConnected) == 0 && on == 1 ? 1 : 0);
        W8(kConnected, on);
    }

    game.Call<0x801450E4>();
    game.Call<0x80142188>();

    if (game.Call<0x803C4878>() == 0)
        game.Call<0x801440E0>();

    game.Call<0x80144818>();
    game.Call<0x80145340>();
    uint32_t listed = 0;

    for (uint32_t i = 0; i < monsters; ++i, monster += 0xB18) {
        if (!R8(monster))
            continue;

        if (game.Call<0x80136C38>({monster}) != 0)
            game.Call<0x80144854>({monster, listed++ & 0xFF});

        if (game.Call<0x803C4878>() == 0 || R8(monster + 0x46B) != 0xFF)
            game.Call<0x80137980>({monster});
        else {
            W8(monster + kActive, 0);
            game.Call<0x80131E58>({monster});
            game.Call<0x80131E18>({monster});
            W16(monster + 0x79C, 1);
        }
    }

    game.Call<0x80145134>();
    game.Call<0x80343D48>();

    if (game.Call<0x803C4878>() == 0) {
        game.Call<0x80145180>();
        game.Call<0x80345188>();
        game.Call<0x80141860>();
        game.Call<0x80141AB0>();
    }
}

void MonstersActive(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    uint32_t monster = game.Call<0x800CFAF4>({3});
    uint32_t hunter = game.Call<0x800CFAF4>({2});
    const uint32_t monsters = game.Call<0x800CFB34>({3}) & 0xFFFF;

    for (uint32_t i = 0; i < monsters; ++i, monster += 0xB18)
        if (R8(monster))
            game.Call<0x80138BC4>({monster});

    const uint32_t hunters = game.Call<0x800CFB34>({2}) & 0xFFFF;

    for (uint32_t i = 0; i < hunters; ++i, hunter += 0xB20)
        if (R8(hunter))
            game.Call<0x80144BC0>({hunter});
}

} // namespace

void MH3_MonsterColoursPort(CpuContext* ctx) {
    Colours(ctx);
}

extern "C" void func_80138BC4(CpuContext* ctx);
extern "C" void MH3_EnemyActiveStep(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyActiveStep", func_80138BC4, ActiveStep);
}
REGISTER_NATIVE_FUNCTION_AS(0x80138BC4, MH3_EnemyActiveStep, "EnemyActiveStep");

extern "C" void func_80143C78(CpuContext* ctx);
extern "C" void MH3_MonstersMove(CpuContext* ctx) {
    PortCheck::Call(ctx, "MonstersMove", func_80143C78, MonstersMove);
}
REGISTER_NATIVE_FUNCTION_AS(0x80143C78, MH3_MonstersMove, "MonstersMove");

extern "C" void func_80143DDC(CpuContext* ctx);
extern "C" void MH3_MonstersActive(CpuContext* ctx) {
    PortCheck::Call(ctx, "MonstersActive", func_80143DDC, MonstersActive);
}
REGISTER_NATIVE_FUNCTION_AS(0x80143DDC, MH3_MonstersActive, "MonstersActive");
