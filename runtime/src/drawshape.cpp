// The game's 2D shape drawer (drawshape.cpp: HUD sprites and icons), RMHP08.
// C reconstructions from the disassembly of the state setters the sprite
// draw (draw_sprite 0x802E07A4) calls per sprite:
//
//   0x80053BCC drawshape_set_texture_rect(u16 texture, const mh_tex_uv* a,
//              const mh_tex_uv* b): the texture and palette of entry
//              `texture` (+0x258 and +0x2DC tables) made current and the
//              rectangle's texture coordinates (s16 texels over the
//              texture's size)
//   0x80053EDC drawshape_set_texture_mtx(pos, scale, u16 angle): the
//              texture matrix: turned by the angle about Z, scaled, and
//              moved by -pos, then pos; concatenated onto the current one
//              (+0x74) when one is set (flag 4)
//
// The state at 0x8066CDA0: u32 flags +0, the coordinates' pointer +0x10, the
// mode words +0xD4/+0xD8 (0x80053A18 sets them by texture kind), the current
// texture +0xE0 and palette +0xE4, the coordinates +0x54..+0x70, the texture
// matrix +0x74, u8 +0x364 the HUD layer's mode.
#include "abi_bridge.h"
#include "fatal.h"
#include "g3d_native.h"
#include "guest_call.h"
#include "memory.h"
#include "nw4r_math.h"
#include "port_check.h"

#include <cstdint>
#include <stdexcept>

namespace {

constexpr uint32_t kState = 0x8066CDA0u;

uint32_t Read32(uint32_t a) {
    return MemoryInline::FlatRead32(a);
}

void Write32(uint32_t a, uint32_t v) {
    MemoryInline::FlatWrite32(a, v);
}

float ReadFloat(uint32_t a) {
    return MemoryInline::FlatReadFloat32(a);
}

void WriteFloat(uint32_t a, float v) {
    MemoryInline::FlatWriteFloat32(a, v);
}

// 0x80053A18: the mode words by texture kind.
void SetKindModes(uint32_t kind) {
    Write32(kState + 0xD8, 0);
    Write32(kState + 0xD4, 0);

    if (MemoryInline::FlatRead8(kState + 0x364) == 0) {
        if (kind == 3 || kind == 6 || kind == 32)
            Write32(kState + 0xD4, 1);
        else if (kind == 4)
            Write32(kState + 0xD8, 1);
    } else {
        if (kind - 10 <= 1 || kind == 21 || kind == 24 || kind == 26)
            Write32(kState + 0xD4, 1);
        else if (kind == 12 || kind == 22)
            Write32(kState + 0xD8, 1);
    }

    Write32(kState, Read32(kState) | 2);
}

void SetTextureRect(uint32_t texture, uint32_t a, uint32_t b) {
    const uint32_t kind = texture & 0xFFFF;
    SetKindModes(kind);
    Write32(kState + 0xE0, Read32(kState + 0x258 + 4 * kind));
    Write32(kState + 0xE4, Read32(kState + 0x2DC + 4 * kind));
    const uint32_t data = Read32(kState + 0xE0);

    if (!data)
        MH3_THROW(std::runtime_error("NW4R panic: ResTex is not valid"));

    const float one = ReadFloat(0x807980C4u); // r2 - 32412
    const float du = one / float(MemoryInline::FlatRead16(data + 0x1C));
    const float dv = one / float(MemoryInline::FlatRead16(data + 0x1E));
    auto s16 = [](uint32_t at) { return float(int16_t(MemoryInline::FlatRead16(at))); };
    const float u0 = s16(a) * du, u1 = s16(b) * du, v0 = s16(a + 2) * dv, v1 = s16(b + 2) * dv;
    WriteFloat(kState + 0x6C, u0);
    WriteFloat(kState + 0x54, u0);
    WriteFloat(kState + 0x64, u1);
    WriteFloat(kState + 0x5C, u1);
    WriteFloat(kState + 0x60, v0);
    WriteFloat(kState + 0x58, v0);
    WriteFloat(kState + 0x70, v1);
    WriteFloat(kState + 0x68, v1);
    Write32(kState + 0x10, kState + 0x54);
}

// `m` and `t` are 0x30-byte guest scratch matrices.
void SetTextureMtx(uint32_t pos, uint32_t scale, uint32_t angle, uint32_t m, uint32_t t) {
    const float c = Nw4rMath::CosU16(angle), s = Nw4rMath::SinU16(angle);
    const float zero = ReadFloat(0x8079F73Cu), one = ReadFloat(0x8079F738u); // PSMTXRotTrig's (r2 - 2084, r2 - 2088)

    // PSMTXRotTrig(m, 'z', s, c), the first column scaled by x and the
    // second by y, the translation pos.
    const float sx = ReadFloat(scale), sy = ReadFloat(scale + 4);
    const float x = ReadFloat(pos), y = ReadFloat(pos + 4);
    const float rows[12] = {c * sx, -s * sy, zero, x, s * sx, c * sy, zero, y, zero, zero, one, zero};

    for (uint32_t k = 0; k < 12; ++k)
        WriteFloat(m + 4 * k, rows[k]);

    // PSMTXTrans(-pos.x, -pos.y, z): identity with that translation.
    const float z = ReadFloat(0x807980B8u); // r2 - 32424
    const float trans[12] = {1.0f, 0.0f, 0.0f, -x, 0.0f, 1.0f, 0.0f, -y, 0.0f, 0.0f, 1.0f, z};

    for (uint32_t k = 0; k < 12; ++k)
        WriteFloat(t + 4 * k, trans[k]);

    G3dNative::MtxConcat(m, t, m);
    const uint32_t current = kState + 0x74;

    if (Read32(kState) & 4) {
        G3dNative::MtxConcat(m, current, current);
    } else {
        G3dNative::MtxCopy(m, current);
        Write32(kState, Read32(kState) | 4);
    }
}

void DrawSprite(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x40);
    GuestCall game{ctx};
    const uint32_t sprite = frame.Arg(3), offset = frame.Arg(4);
    auto s16 = [](uint32_t at) { return int32_t(int16_t(MemoryInline::FlatRead16(at))); };
    int16_t x = int16_t(s16(sprite)), y = int16_t(s16(sprite + 2));
    const uint32_t cornerA = frame[0xC], cornerB = frame[8], center = frame[0x20], scale = frame[0x18];
    Write32(cornerA, Read32(sprite + 0x14));
    Write32(cornerB, Read32(sprite + 0x18));

    if (offset) {
        x = int16_t(x + s16(offset));
        y = int16_t(y + s16(offset + 2));
    }

    WriteFloat(center, float(x));
    WriteFloat(center + 4, float(y));
    x = int16_t(x + s16(sprite + 4));
    y = int16_t(y + s16(sprite + 6));
    const float unit = ReadFloat(0x8079CD84u); // r2 - 12764
    WriteFloat(scale, float(MemoryInline::FlatRead16(sprite + 0xC)) / unit);
    WriteFloat(scale + 4, float(MemoryInline::FlatRead16(sprite + 0xE)) / unit);
    game.Call<0x80053870>({3, 0xFFFF});
    game.Call<0x800538E0>({uint32_t(int32_t(x)), uint32_t(int32_t(y)), uint32_t(s16(sprite + 8)), uint32_t(s16(sprite + 0xA))});
    game.Call<0x8005399C>({Read32(sprite + 0x1C)});
    SetTextureRect(MemoryInline::FlatRead8(sprite + 0x12), cornerA, cornerB);
    const uint32_t sp = frame[0];
    SetTextureMtx(center, scale, MemoryInline::FlatRead16(sprite + 0x10), sp - 0x90 + 0x38, sp - 0x90 + 8);
    game.Call<0x8005480C>();
}

} // namespace

extern "C" void func_802E07A4(CpuContext* ctx);
extern "C" void MH3_DrawSprite(CpuContext* ctx) {
    PortCheck::Call(ctx, "DrawSprite", func_802E07A4, DrawSprite);
}
REGISTER_NATIVE_FUNCTION_AS(0x802E07A4, MH3_DrawSprite, "DrawSprite");

extern "C" void func_80053BCC(CpuContext* ctx);
static void SetTextureRectPort(CpuContext* p) {
    SetTextureRect(p->gpr[3], p->gpr[4], p->gpr[5]);
}

static void SetTextureRectChecked([[maybe_unused]] uint32_t r1, uint32_t r3, uint32_t r4, uint32_t r5) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Enabled("DrawShapeSetTextureRect")) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[1] = r1;
        c->gpr[2] = 0x8079FF60u;
        c->gpr[3] = r3;
        c->gpr[4] = r4;
        c->gpr[5] = r5;
        PortCheck::RunOutput(c, "DrawShapeSetTextureRect", func_80053BCC, SetTextureRectPort, {{kState, 0x370}});
        return;
    }
#endif
    SetTextureRect(r3, r4, r5);
}

extern "C" void MH3_DrawShapeSetTextureRect(CpuContext* ctx) {
    SetTextureRectChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4], ctx->gpr[5]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80053BCC, MH3_DrawShapeSetTextureRect, "DrawShapeSetTextureRect");

// The matrices go where the original's frame keeps them (r1 - 0x90 + 0x38 and
// + 8), below the caller's stack pointer.
extern "C" void func_80053EDC(CpuContext* ctx);
static void SetTextureMtxAt(uint32_t r1, uint32_t r3, uint32_t r4, uint32_t r5) {
    SetTextureMtx(r3, r4, r5, r1 - 0x90 + 0x38, r1 - 0x90 + 8);
}

static void SetTextureMtxPort(CpuContext* p) {
    SetTextureMtxAt(p->gpr[1], p->gpr[3], p->gpr[4], p->gpr[5]);
}

static void SetTextureMtxChecked(uint32_t r1, uint32_t r3, uint32_t r4, uint32_t r5) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Enabled("DrawShapeSetTextureMtx")) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[1] = r1;
        c->gpr[2] = 0x8079FF60u;
        c->gpr[3] = r3;
        c->gpr[4] = r4;
        c->gpr[5] = r5;
        PortCheck::RunOutput(c, "DrawShapeSetTextureMtx", func_80053EDC, SetTextureMtxPort, {{kState, 0xA4}});
        return;
    }
#endif
    SetTextureMtxAt(r1, r3, r4, r5);
}

extern "C" void MH3_DrawShapeSetTextureMtx(CpuContext* ctx) {
    SetTextureMtxChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4], ctx->gpr[5]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80053EDC, MH3_DrawShapeSetTextureMtx, "DrawShapeSetTextureMtx");
