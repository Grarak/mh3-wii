// A character model's material state, RMHP08. C reconstructions from the
// disassembly (a material's channel colour, 0x800E2950 and 0x800E2B58, is in
// effect_objects.cpp):
//
//   0x800E2FA4 (ch, material, compare0, ref0, operation, compare1, ref1):
//              the alpha test, in the model's copy of the material (MHchar
//              +0x118 the ScnMdl, nothing without one)
//   0x800E2948 (ch): the model's material count
//   0x800E2A0C (ch, channel, GXColor* colour, dirty): every material's colour
//   0x800E2A84 (ch, channel, u8 alpha, dirty): every material's alpha, its
//              pixel state the resource's at 255, else blended
//   0x800767C8 ResMatPix of a ResMat, 0x800767F8 its constructor (dropping
//              the alignment assert)
//   0x8007BE10 CopiedMatAccess::GetResMatPix(access, dirty): the pixel copy,
//              null without one; dirty marks it (0x8007BBF8, bit 0x80)
//   0x8007E4EC the pixel copy's flush (0x80094180)
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"

#include <cstdint>

namespace {

enum : uint32_t {
    CopiedMatAccess = 0x8007BF10, // (access, mdl, material)
    AccessValid = 0x800E29F8,     // (access)
    MaterialCount = 0x800E2948,   // (ch)
    SetColourAt = 0x800E2950,
    GetColourAt = 0x800E2B58,
    Refresh = 0x800E3140, // (ch, material, bits)
    Blend = 0x800E308C,   // (ch, material, ...)
};

uint32_t R32(uint32_t a) {
    return MemoryInline::FlatRead32(a);
}

void W32(uint32_t a, uint32_t v) {
    MemoryInline::FlatWrite32(a, v);
}

void AlphaTest(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x70);
    GuestCall game{ctx};
    const uint32_t access = frame[16], pixel = frame[12], word = frame[8], model = R32(frame.Arg(3) + 0x118);

    if (!model)
        return;

    game.Call<CopiedMatAccess>({access, model, frame.Arg(4)});

    if (game.Call<AccessValid>({access}) == 0)
        return;

    W32(word, game.Call<0x8007BE10>({access, 0}));
    game.Call<0x800E3050>({pixel, word});
    game.Call<0x80095F94>({pixel, frame.Arg(5), frame.Arg(6) & 0xFF, frame.Arg(7), frame.Arg(8), frame.Arg(9) & 0xFF}); // MatAlphaCompare
    game.Call<0x8007E4EC>({pixel});
}

void SetColours(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x40);
    GuestCall game{ctx};
    const uint32_t ch = frame.Arg(3), copy = frame[8];
    const uint32_t count = game.Call<MaterialCount>({ch});

    for (uint32_t i = 0; i < count; ++i) {
        W32(copy, R32(frame.Arg(5)));
        game.Call<SetColourAt>({ch, i, frame.Arg(4), copy, frame.Arg(6)});
    }
}

void SetAlphas(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x40);
    GuestCall game{ctx};
    const uint32_t ch = frame.Arg(3), channel = frame.Arg(4), alpha = frame.Arg(5) & 0xFF, colour = frame[12], copy = frame[8];
    const uint32_t count = game.Call<MaterialCount>({ch});

    for (uint32_t i = 0; i < count; ++i) {
        if (game.Call<GetColourAt>({ch, i, channel, colour}) == 0)
            continue;

        MemoryInline::FlatWrite8(colour + 3, uint8_t(alpha));
        W32(copy, R32(colour));
        game.Call<SetColourAt>({ch, i, channel, copy, frame.Arg(6)});

        if (alpha == 0xFF)
            game.Call<Refresh>({ch, i, 0x80});
        else
            game.Call<Blend>({ch, i, 1, 4, 5, 0});
    }
}

} // namespace

extern "C" void func_800E2FA4(CpuContext* ctx);
extern "C" void MH3_CharAlphaTest(CpuContext* ctx) {
    PortCheck::Call(ctx, "CharAlphaTest", func_800E2FA4, AlphaTest);
}
REGISTER_NATIVE_FUNCTION_AS(0x800E2FA4, MH3_CharAlphaTest, "CharAlphaTest");

extern "C" void func_800E2A0C(CpuContext* ctx);
extern "C" void MH3_CharSetColours(CpuContext* ctx) {
    PortCheck::Call(ctx, "CharSetColours", func_800E2A0C, SetColours);
}
REGISTER_NATIVE_FUNCTION_AS(0x800E2A0C, MH3_CharSetColours, "CharSetColours");

extern "C" void func_800E2A84(CpuContext* ctx);
extern "C" void MH3_CharSetAlphas(CpuContext* ctx) {
    PortCheck::Call(ctx, "CharSetAlphas", func_800E2A84, SetAlphas);
}
REGISTER_NATIVE_FUNCTION_AS(0x800E2A84, MH3_CharSetAlphas, "CharSetAlphas");

extern "C" void MH3_CharMaterialCount(CpuContext* ctx) {
    uint32_t count;
    {
        GuestFrame frame(ctx, 0x20);
        GuestCall game{ctx};
        count = game.Call<0x800988CC>({frame.Arg(3) + 0x114});
    }

    ctx->gpr[3] = count;
}
REGISTER_NATIVE_FUNCTION_AS(0x800E2948, MH3_CharMaterialCount, "CharMaterialCount");

extern "C" void MH3_ResMatPixel(CpuContext* ctx) {
    uint32_t pixel;
    {
        GuestFrame frame(ctx, 0x20);
        GuestCall game{ctx};
        pixel = game.Call<0x8006F228>({frame.Arg(3)});
    }

    ctx->gpr[3] = pixel;
}
REGISTER_NATIVE_FUNCTION_AS(0x800767C8, MH3_ResMatPixel, "ResMatPixel");

extern "C" void MH3_ResMatPixelMake(CpuContext* ctx) {
    W32(ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800767F8, MH3_ResMatPixelMake, "ResMatPixelMake");

extern "C" void func_8007BE10(CpuContext* ctx);
extern "C" void MH3_CopiedMatPixel(CpuContext* ctx) {
    PortCheck::Call(ctx, "CopiedMatPixel", func_8007BE10, [](CpuContext* c) {
        uint32_t pixel;
        {
            GuestFrame frame(c, 0x20);
            GuestCall game{c};
            const uint32_t access = frame.Arg(3), mdl = R32(access);
            pixel = mdl ? R32(access + 0x20) : 0;

            if (pixel && frame.Arg(4))
                game.Call<0x8007BBF8>({mdl, R32(access + 4), 0x80});
        }

        c->gpr[3] = pixel;
    });
}
REGISTER_NATIVE_FUNCTION_AS(0x8007BE10, MH3_CopiedMatPixel, "CopiedMatPixel");

extern "C" void MH3_CopiedMatFlush(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    game.Call<0x80094180>({frame.Arg(3), 0});
}
REGISTER_NATIVE_FUNCTION_AS(0x8007E4EC, MH3_CopiedMatFlush, "CopiedMatFlush");
