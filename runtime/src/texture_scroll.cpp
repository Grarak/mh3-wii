// Scrolling texture effect matrices, RMHP08. Three per-step updates move a
// material's texture effect matrix (its translation, a UV offset) on by a
// constant each step: the matrix itself is the accumulator, read with
// ResMatTexCoordGen::GetEffectMtx and written back with SetEffectMtx. Here the
// offset moves on by that constant times the step length, so the textures
// scroll at the same speed at any step length. At the stock 1/30 s step each
// is the original.
//
//   0x803750E8 sea surface (the Ceadeus lair's open sea, its intro cutscene):
//     materials 8 and 9 of the model at +316: V offset + 0.0025 (sda2 -7740)
//     a step, back by 1 past 1; U offset a sine of the tick counter
//     (system_w+0xC << 7). Then TEV colours: material 6 register 3 from the
//     bytes at +825..827, and register 0 of materials 0-3 white when +481
//     is set and the act (+485/+486, 0x8012D270) is not 13/2, else grey 150.
//   0x801A9274 when the kind at +480 (through the map at 0x805CF500, 255: as
//     is) is 6 and +481 is 1: material 6 of the model at +316, U offset
//     - const (sda2 -21892) a step, + wrap (-21880) below the limit (-21864).
//     Then material 6 TEV colour register 4 (0x800E2B58 reads it): alpha =
//     const (-20920) * f32 +468, or 0 when not kind 6 / +481 1, written with
//     0x800E2950.
//   0x8031ED54 per step for each object of the list at +56 (count, then
//     pointers from +4), the parts of effect 45 (eft045_set 0x8031EBB8): the
//     object's model (res_eft_model_create 0x800F8EC0, 67, 268); without it
//     the pass ends (0x8031F364, skipping 0x8031F050); else it places the object (0x80041EA4, even
//     objects at +36, odd ones through 0x80051278 / 0x80051454 / 0x80050D7C),
//     scales +28 by a random 0..19 step (0x800CEF20, the game's RNG 0), sets
//     +48 to another random, +56 = 1, +53 = 1, and scrolls material 0 of the
//     model at +280: U offset - const (sda2 -11224) a step, wrapped into
//     [-11240, -11244). The byte at +5 counts passes; the list's s16 at +22 is
//     the object index.
//
// ResMatTexCoordGen (nw4r g3d): the handle's data (0x8006E310, 0x800878D4 its
// null check) holds 8 texture coordinate entries of 52 bytes; +171 flags (bit
// 0: identity), +172 the 3x4 effect matrix.
//   0x80095434 GetEffectMtx(handle, index, out): copies entry index's matrix.
//   0x800953A8 SetEffectMtx(handle, index, mtx): stores it (identity for
//     null, setting the flag). Both return 0 for an index past 7.
#include "game_step.h"
#include "guest_call.h"
#include "port_check.h"

#include <algorithm>
#include <cmath>

namespace {

enum : uint32_t {
    GetResMat = 0x8007BF10,         // (out, model, material index)
    ResMatTexCoordGen = 0x8007BC10, // (resmat, 0) -> handle data
    HandleCopy = 0x8006F368,        // (out, in): *out = *in
    TexCoordGenData = 0x800878D4,   // null check + data (panics on null)
    TexCoordGenData2 = 0x8006E310,  // the same from SetEffectMtx
    SinIdx = 0x800502B0,            // (u16 index) -> f1
    GetTevColor = 0x800E2D38,       // (mdl, material, register, out)
    SetTevColor = 0x800E2DC4,       // (mdl, material, register, color)
    GetTevKColor = 0x800E2B58,      // (mdl, material, register, out) -> 1 if present
    SetTevKColor = 0x800E2950,      // (mdl, material, register, color, 0)
    CreateModel = 0x800F8EC0,       // res_eft_model_create (MHchar, 67, 268) -> 0 when it failed
    EndPass = 0x8031F364,           // (this)
    FinishPass = 0x8031F050,        // (this)
    CopyPosition = 0x80041EA4,      // (dst, src)
    RotateZXY = 0x80051278,         // rotVecZXY (v, CP_VECTOR angles)
    VecScaleBy = 0x80051454,        // (v, f1)
    VecAngles = 0x80050D7C,         // calcVecAngXY (v, out x, out y)
};

constexpr uint32_t r2 = 0x8079FF60;           // the sda2 base
constexpr uint32_t kTickCounter = 0x8065AA6C; // system_w+0xC (ticks, step_counter.cpp)
constexpr uint32_t kRandSeed = 0x8065AA78;    // system_w+0x18: RNG 0 (s16)
constexpr uint32_t kKindMap = 0x805CF500;     // u8[256], 255: unmapped
constexpr uint32_t kEntrySize = 52, kEntryFlags = 171, kEntryMtx = 172;

float F(uint32_t a) {
    return Memory::ReadFloat32(a);
}

void F(uint32_t a, float v) {
    Memory::WriteFloat32(a, v);
}

float Step() {
    return float(GameStep::Scale());
}

void CopyMtx(uint32_t src, uint32_t dst) {
    for (uint32_t i = 0; i < 48; i += 4)
        Memory::Write32(dst + i, Memory::Read32(src + i));
}

// The handle's data: its null check panics as the original's does.
uint32_t Data(CpuContext* ctx, uint32_t handle, uint32_t check) {
    const uint32_t data = Memory::Read32(handle);

    if (data) [[likely]]
        return data;
    GuestCall game{ctx};
    game(check, {handle});
    return Memory::Read32(handle);
}

uint32_t GetEffectMtx(CpuContext* ctx, uint32_t handle, uint32_t index, uint32_t out) {
    if (!out || index >= 8)
        return 0;

    CopyMtx(Data(ctx, handle, TexCoordGenData) + index * kEntrySize + kEntryMtx, out);
    return 1;
}

uint32_t SetEffectMtx(CpuContext* ctx, uint32_t handle, uint32_t index, uint32_t mtx) {
    if (index >= 8)
        return 0;

    const uint32_t entry = Data(ctx, handle, TexCoordGenData2) + index * kEntrySize;

    if (mtx) {
        CopyMtx(mtx, entry + kEntryMtx);
        Memory::Write8(entry + kEntryFlags, Memory::Read8(entry + kEntryFlags) & 0xFE);
    } else {
        for (uint32_t i = 0; i < 12; ++i)
            F(entry + kEntryMtx + 4 * i, i % 5 == 0 ? 1.0f : 0.0f);

        Memory::Write8(entry + kEntryFlags, Memory::Read8(entry + kEntryFlags) | 1);
    }

    return 1;
}

// A material's texture coordinate handle in `handle` (4 bytes), through the
// scratch word `scratch`; false when the model has no such material.
bool MaterialTexCoords(GuestCall& game, uint32_t resmat, uint32_t scratch, uint32_t handle) {
    if (!Memory::Read32(resmat))
        return false;

    Memory::Write32(scratch, game.Call<ResMatTexCoordGen>({resmat, 0}));
    game.Call<HandleCopy>({handle, scratch});
    return true;
}

// 0x800CEF20 for RNG 0: seed * 176 mod 65363 (a zero seed counts as 1).
uint32_t Rand() {
    uint32_t seed = Memory::Read16(kRandSeed);

    if (!seed)
        seed = 1;

    seed = seed * 176 % 65363;
    Memory::Write16(kRandSeed, uint16_t(seed));
    return seed;
}

void SeaSurfacePort(CpuContext* ctx) {
    GuestFrame frame(ctx, 192);
    GuestCall game{ctx};
    const uint32_t self = frame.Arg(3);
    const uint32_t model = Memory::Read32(self + 316);
    const uint32_t mat8 = frame[80], mat9 = frame[28], mtx = frame[136];
    game.Call<GetResMat>({mat8, model, 8});
    game.Call<GetResMat>({mat9, model, 9});

    if (Memory::Read32(mat8) && Memory::Read32(mat9)) {
        MaterialTexCoords(game, mat8, frame[12], frame[20]);
        MaterialTexCoords(game, mat9, frame[8], frame[16]);
        GetEffectMtx(ctx, frame[20], 1, mtx);
        game.Call<SinIdx>({(Memory::Read32(kTickCounter) << 7) & 0xFF80});
        F(mtx + 12, F(r2 - 8660) * game.F1());
        float v = F(mtx + 28) + F(r2 - 7740) * Step();

        if (v > F(r2 - 8792))
            v -= F(r2 - 8792);

        F(mtx + 28, v);
        SetEffectMtx(ctx, frame[20], 1, mtx);
        SetEffectMtx(ctx, frame[16], 1, mtx);
    }

    const uint32_t mdl = self + 36, color = frame[24];
    game.Call<GetTevColor>({mdl, 6, 3, color});

    for (uint32_t i = 0; i < 3; ++i)
        Memory::Write8(color + i, Memory::Read8(self + 825 + i));

    game.Call<SetTevColor>({mdl, 6, 3, color});
    game.Call<GetTevColor>({mdl, 0, 0, color});
    const bool white = Memory::Read8(self + 481) && !(Memory::Read8(self + 485) == 13 && Memory::Read8(self + 486) == 2);

    for (uint32_t i = 0; i < 3; ++i)
        Memory::Write8(color + i, white ? 255 : 150);

    for (uint32_t reg = 0; reg < 4; ++reg)
        game.Call<SetTevColor>({mdl, reg, 0, color});
}

void Mat6ScrollPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 144);
    GuestCall game{ctx};
    const uint32_t self = frame.Arg(3), mdl = self + 36, color = frame[24];
    uint32_t kind = Memory::Read8(self + 480);

    if (const uint32_t mapped = Memory::Read8(kKindMap + kind); mapped != 255)
        kind = mapped;

    if (kind == 6 && Memory::Read8(self + 481) == 1) {
        const uint32_t mat = frame[28], mtx = frame[80];
        game.Call<GetResMat>({mat, Memory::Read32(self + 316), 6});

        if (MaterialTexCoords(game, mat, frame[16], frame[20])) {
            GetEffectMtx(ctx, frame[20], 0, mtx);
            float u = F(mtx + 12) - F(r2 - 21892) * Step();

            if (u < F(r2 - 21864))
                u += F(r2 - 21880);

            F(mtx + 12, u);
            SetEffectMtx(ctx, frame[20], 0, mtx);
        }

        if (game.Call<GetTevKColor>({mdl, 6, 4, color}) == 1) {
            const float alpha = F(r2 - 20920) * F(self + 468);
            Memory::Write8(color + 3, uint8_t(int32_t(std::clamp(alpha, -2147483648.0f, 2147483520.0f))));
            Memory::Write32(frame[12], Memory::Read32(color));
            game.Call<SetTevKColor>({mdl, 6, 4, frame[12], 0});
        }
    } else if (game.Call<GetTevKColor>({mdl, 6, 4, color}) == 1) {
        Memory::Write8(color + 3, 0);
        Memory::Write32(frame[8], Memory::Read32(color));
        game.Call<SetTevKColor>({mdl, 6, 4, frame[8], 0});
    }
}

void ObjectListPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 288);
    GuestCall game{ctx};
    const uint32_t self = frame.Arg(3);
    const uint32_t list = Memory::Read32(self + 56);
    Memory::Write8(self + 5, uint8_t(Memory::Read8(self + 5) + 1));
    Memory::Write16(list + 22, 0);
    const float wide = F(r2 - 11248), one = F(r2 - 11244), low = F(r2 - 11240), spread = F(r2 - 11236);
    const float scaleStep = F(r2 - 11228), scaleBase = F(r2 - 11232), speed = F(r2 - 11224);

    for (int32_t i = 0; int32_t(int16_t(i)) < int32_t(Memory::Read32(list)); ++i) {
        const uint32_t obj = Memory::Read32(list + 4 + 4 * uint32_t(i));

        if (game.Call<CreateModel>({obj, 67, 268}) == 0) {
            game.Call<EndPass>({self});
            return;
        }

        game.Call<CopyPosition>({obj + 4, self + 24});

        if ((int16_t(Memory::Read16(list + 22)) & 1) == 0) {
            for (uint32_t k = 0; k < 12; k += 4)
                Memory::Write32(obj + 40 + k, Memory::Read32(self + 36 + k));
        } else {
            const uint32_t v = frame[16];
            F(v, low), F(v + 4, low), F(v + 8, one);
            game.Call<RotateZXY>({v, self + 36});
            game.Call<VecScaleBy>({v}, {spread});
            game.Call<VecAngles>({v, obj + 40, obj + 44});
            Memory::Write32(obj + 48, 0);
        }

        F(obj + 28, wide), F(obj + 32, wide), F(obj + 36, one);
        game.Call<VecScaleBy>({obj + 28}, {scaleBase + scaleStep * float(int32_t(Rand() % 20))});
        Memory::Write32(obj + 48, Rand());
        Memory::Write32(obj + 56, 1);
        Memory::Write8(obj + 53, 1);
        Memory::Write16(list + 16 + 2 * uint32_t(i), uint16_t(int16_t(-6 * i)));
        const uint32_t mat = frame[80], mtx = frame[32];
        game.Call<GetResMat>({mat, Memory::Read32(obj + 280), 0});

        if (MaterialTexCoords(game, mat, frame[8], frame[12])) {
            GetEffectMtx(ctx, frame[12], 0, mtx);
            float u = F(mtx + 12) - speed * Step();

            if (u < low)
                u += one;

            if (u >= one)
                u -= one;

            F(mtx + 12, u);
            SetEffectMtx(ctx, frame[12], 0, mtx);
        }

        Memory::Write16(list + 22, uint16_t(int16_t(Memory::Read16(list + 22)) + 1));
    }

    game.Call<FinishPass>({self});
}

void GetEffectMtxPort(CpuContext* ctx) {
    ctx->gpr[3] = GetEffectMtx(ctx, ctx->gpr[3], ctx->gpr[4], ctx->gpr[5]);
}

void SetEffectMtxPort(CpuContext* ctx) {
    ctx->gpr[3] = SetEffectMtx(ctx, ctx->gpr[3], ctx->gpr[4], ctx->gpr[5]);
}

} // namespace

extern "C" void func_80095434(CpuContext* ctx);
extern "C" void MH3_TexCoordGetEffectMtx(CpuContext* ctx) {
    PortCheck::Call(ctx, "TexCoordGetEffectMtx", func_80095434, GetEffectMtxPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80095434, MH3_TexCoordGetEffectMtx, "TexCoordGetEffectMtx");

extern "C" void func_800953A8(CpuContext* ctx);
extern "C" void MH3_TexCoordSetEffectMtx(CpuContext* ctx) {
    PortCheck::Call(ctx, "TexCoordSetEffectMtx", func_800953A8, SetEffectMtxPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x800953A8, MH3_TexCoordSetEffectMtx, "TexCoordSetEffectMtx");

extern "C" void func_803750E8(CpuContext* ctx);
extern "C" void MH3_SeaSurfaceUpdate(CpuContext* ctx) {
    PortCheck::Call(ctx, "SeaSurfaceUpdate", func_803750E8, SeaSurfacePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803750E8, MH3_SeaSurfaceUpdate, "SeaSurfaceUpdate");

extern "C" void func_801A9274(CpuContext* ctx);
extern "C" void MH3_Mat6ScrollUpdate(CpuContext* ctx) {
    PortCheck::Call(ctx, "Mat6ScrollUpdate", func_801A9274, Mat6ScrollPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801A9274, MH3_Mat6ScrollUpdate, "Mat6ScrollUpdate");

extern "C" void func_8031ED54(CpuContext* ctx);
extern "C" void MH3_ObjectListScrollUpdate(CpuContext* ctx) {
    PortCheck::Call(ctx, "ObjectListScrollUpdate", func_8031ED54, ObjectListPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8031ED54, MH3_ObjectListScrollUpdate, "ObjectListScrollUpdate");
