// NW4R g3d AnmObjChrRes and FrameCtrl, RMHP08. C reconstructions from the
// disassembly of the frame setter (0x80061D90), frame getter (0x80061F6C),
// per-step update (0x8006236C), result lookup (0x800631D4) and result cache
// update (0x800634A0) of a character animation object, and of FrameCtrl's
// SetFrame (0x80061F28) and UpdateFrm (0x800624B0).
//
// AnmObjChrRes (vtable 0x8058C468):
//   +0x10 u32 the model's node count, +0x14 u16[count] bindings: model node ->
//   animation node (bits 0-13); bit 15 unbound, bit 14 bound but without a
//   result; +0x18 FrameCtrl; +0x2C ResAnmChr; +0x30 the result cache, one
//   76-byte ChrAnmResult per animation node at the current frame, or 0.
// FrameCtrl: +0 frame, +4 rate, +8 start, +0xC end, +0x10 the play policy,
//   which maps (start, end, frame) to the frame stored (clamp or loop).
//
// With a cache, setting or advancing the frame evaluates every bound node
// (ResAnmChr::GetAnmResult, anm_chr_keys.cpp) and GetResult hands out the
// cached entry; without one GetResult evaluates the node at the frame.
// AnmObjChrResResultAt (anm_obj_chr_res.h) evaluates one node at any frame
// without touching the object.
#include "anm_obj_chr_res.h"
#include "port_check.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {

enum : uint32_t {
    kCount = 0x10,
    kBindings = 0x14,
    kFrameCtrl = 0x18,
    kRes = 0x2C,
    kCache = 0x30,
    kResultBytes = 76,
    // FrameCtrl
    kFrame = 0x0,
    kRate = 0x4,
    kStart = 0x8,
    kEnd = 0xC,
    kPolicy = 0x10,
};
constexpr uint32_t kGetAnmResult = 0x8008D660; // ResAnmChr::GetAnmResult(res, result, node, frame)

float F32(uint32_t address) {
    return MemoryInline::FlatReadFloat32(address);
}
uint32_t Bits(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    return bits;
}

// NW4R's pointer check (MEM1, MEM2, their uncached views, the locked cache).
bool ValidPointer(uint32_t p) {
    return (p & 0xFF000000u) == 0x80000000u || (p & 0xFF800000u) == 0x81000000u || (p & 0xF8000000u) == 0x90000000u || (p & 0xFF000000u) == 0xC0000000u || (p & 0xFF800000u) == 0xC1000000u ||
           (p & 0xF8000000u) == 0xD0000000u || (p & 0xFFFFC000u) == 0xE0000000u;
}
// nw4r::db::Panic (0x80501130, g3d_anmchr.cpp) halts the game; so does the port.
[[noreturn]] void Panic(const char* what, uint32_t line, uint32_t value) {
    char text[128];
    std::snprintf(text, sizeof text, "NW4R panic (g3d_anmchr.cpp:%u): %s (%08x)", line, what, value);
    throw std::runtime_error(text);
}
void CheckPointer(uint32_t p, uint32_t line) {
    if (!ValidPointer(p)) [[unlikely]]
        Panic("Pointer Error", line, p);
}

// FrameCtrl::SetFrame: the frame the play policy makes of `frame`.
float PolicyFrame(GuestCall& game, uint32_t control, float frame) {
    game(Memory::Read32(control + kPolicy), {}, {F32(control + kStart), F32(control + kEnd), frame});
    return game.F1();
}
void SetControlFrame(GuestCall& game, uint32_t control, float frame) {
    Memory::WriteFloat32(control + kFrame, PolicyFrame(game, control, frame));
}

void UpdateCache(GuestCall& game, uint32_t self) {
    CheckPointer(self, 1326);
    const uint32_t cache = Memory::Read32(self + kCache);
    CheckPointer(cache, 1327);
    const float frame = F32(self + kFrameCtrl + kFrame);
    const uint32_t count = Memory::Read32(self + kCount), bindings = Memory::Read32(self + kBindings);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t binding = MemoryInline::FlatRead16(bindings + 2 * i);
        if (binding & 0x8000)
            continue;
        const uint32_t node = binding & 0x3FFF;
        game.Call<kGetAnmResult>({self + kRes, cache + kResultBytes * node, node}, {frame});
    }
}

void SetFrame(GuestCall& game, uint32_t self, float frame) {
    CheckPointer(self, 1049);
    if (!std::isfinite(frame)) [[unlikely]]
        Panic("Floating Point Value Error frame", 1050, Bits(frame));
    SetControlFrame(game, self + kFrameCtrl, frame);
    if (Memory::Read32(self + kCache))
        UpdateCache(game, self);
}

void UpdateFrame(GuestCall& game, uint32_t self, uint32_t r2, uint32_t r13) {
    CheckPointer(self, 1096);
    const uint32_t control = self + kFrameCtrl;
    if (F32(control + kRate) == F32(r2 - 32088)) // 0.0f
        return;
    // FrameCtrl::UpdateFrm: frame + rate * smBaseUpdateRate, each rounded to single.
    const float advance = F32(control + kRate) * F32(r13 - 31928);
    SetControlFrame(game, control, F32(control + kFrame) + advance);
    if (Memory::Read32(self + kCache))
        UpdateCache(game, self);
}

uint32_t GetResult(GuestCall& game, uint32_t self, uint32_t result, uint32_t index) {
    CheckPointer(self, 1297);
    if (int32_t(index) > int32_t(Memory::Read32(self + kCount)) - 1) [[unlikely]]
        Panic("Failed assertion idx < count", 1298, index);
    CheckPointer(result, 1299);
    const uint32_t binding = MemoryInline::FlatRead16(Memory::Read32(self + kBindings) + 2 * index);
    if (binding & 0xC000) {
        MemoryInline::FlatWrite32(result, 0);
        return result;
    }
    if (const uint32_t cache = Memory::Read32(self + kCache))
        return cache + kResultBytes * binding;
    game.Call<kGetAnmResult>({self + kRes, result, binding}, {F32(self + kFrameCtrl + kFrame)});
    return result;
}

// The registered bodies: guest frames as small as the originals' (32 or 16
// bytes); the f1 and r3 results are set after the frame restores the GPRs.
// They are registered kNativeKeepsNonvolatile (no dispatch guard around them):
// they name no r14-r31 or f14-f31, the frames restore every GPR, and their
// guest calls go through GuestCall, whose dispatch guards a callee that may
// change f14-f31.
void SetFramePort(CpuContext* ctx) {
    GuestFrame frame(ctx, 32);
    GuestCall game{ctx};
    SetFrame(game, frame.Arg(3), float(ctx->fpr[1].d));
}
void GetFramePort(CpuContext* ctx) {
    CheckPointer(ctx->gpr[3], 1064);
    ctx->fpr[1].d = F32(ctx->gpr[3] + kFrameCtrl + kFrame);
}
void UpdateFramePort(CpuContext* ctx) {
    GuestFrame frame(ctx, 16);
    GuestCall game{ctx};
    UpdateFrame(game, frame.Arg(3), frame.Arg(2), frame.Arg(13));
}
void GetResultPort(CpuContext* ctx) {
    uint32_t out;
    {
        GuestFrame frame(ctx, 32);
        GuestCall game{ctx};
        out = GetResult(game, frame.Arg(3), frame.Arg(4), frame.Arg(5));
    }
    ctx->gpr[3] = out;
}
void UpdateCachePort(CpuContext* ctx) {
    GuestFrame frame(ctx, 32);
    GuestCall game{ctx};
    UpdateCache(game, frame.Arg(3));
}
void ControlSetFramePort(CpuContext* ctx) {
    float stored;
    {
        GuestFrame frame(ctx, 16);
        GuestCall game{ctx};
        SetControlFrame(game, frame.Arg(3), float(ctx->fpr[1].d));
        stored = F32(frame.Arg(3) + kFrame);
    }
    ctx->fpr[1].d = stored;
}
void ControlUpdateFrmPort(CpuContext* ctx) {
    const uint32_t control = ctx->gpr[3];
    ctx->fpr[1].d = F32(control + kFrame) + F32(control + kRate) * F32(ctx->gpr[13] - 31928);
    ControlSetFramePort(ctx);
}

} // namespace

uint32_t AnmObjChrResResultAt(GuestCall& game, uint32_t anim, uint32_t result, uint32_t index, float frame) {
    CheckPointer(anim, 1049);
    if (!std::isfinite(frame)) [[unlikely]]
        Panic("Floating Point Value Error frame", 1050, Bits(frame));
    CheckPointer(result, 1299);
    if (int32_t(index) > int32_t(Memory::Read32(anim + kCount)) - 1) [[unlikely]]
        Panic("Failed assertion idx < count", 1298, index);
    const uint32_t binding = MemoryInline::FlatRead16(Memory::Read32(anim + kBindings) + 2 * index);
    if (binding & 0xC000) {
        MemoryInline::FlatWrite32(result, 0);
        return result;
    }
    const float at = PolicyFrame(game, anim + kFrameCtrl, frame);
    game.Call<kGetAnmResult>({anim + kRes, result, binding}, {at});
    return result;
}

#ifdef MH3_DEVELOPMENT
// With MH3_PORT_CHECK naming the port, it runs checked against the original.
#define MH3_ANM_OBJ_CHECK(name, original, port)                                                                                                                                                       \
    const bool check = PortCheck::Enabled(name);                                                                                                                                                       \
    if (check) [[unlikely]] {                                                                                                                                                                          \
        PortCheck::Run(ctx, name, original, port);                                                                                                                                                     \
        return;                                                                                                                                                                                        \
    }
#else
#define MH3_ANM_OBJ_CHECK(name, original, port)
#endif

extern "C" void func_80061D90(CpuContext* ctx);
extern "C" void MH3_AnmObjChrResSetFrame(CpuContext* ctx) {
    MH3_ANM_OBJ_CHECK("AnmObjChrResSetFrame", func_80061D90, SetFramePort)
    SetFramePort(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x80061D90, MH3_AnmObjChrResSetFrame, "AnmObjChrResSetFrame", kNativeKeepsNonvolatile);

extern "C" void func_80061F28(CpuContext* ctx);
extern "C" void MH3_FrameCtrlSetFrame(CpuContext* ctx) {
    MH3_ANM_OBJ_CHECK("FrameCtrlSetFrame", func_80061F28, ControlSetFramePort)
    ControlSetFramePort(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x80061F28, MH3_FrameCtrlSetFrame, "FrameCtrlSetFrame", kNativeKeepsNonvolatile);

extern "C" void func_80061F6C(CpuContext* ctx);
extern "C" void MH3_AnmObjChrResGetFrame(CpuContext* ctx) {
    MH3_ANM_OBJ_CHECK("AnmObjChrResGetFrame", func_80061F6C, GetFramePort)
    GetFramePort(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x80061F6C, MH3_AnmObjChrResGetFrame, "AnmObjChrResGetFrame", kNativeKeepsNonvolatile);

extern "C" void func_8006236C(CpuContext* ctx);
extern "C" void MH3_AnmObjChrResUpdateFrame(CpuContext* ctx) {
    MH3_ANM_OBJ_CHECK("AnmObjChrResUpdateFrame", func_8006236C, UpdateFramePort)
    UpdateFramePort(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x8006236C, MH3_AnmObjChrResUpdateFrame, "AnmObjChrResUpdateFrame", kNativeKeepsNonvolatile);

extern "C" void func_800624B0(CpuContext* ctx);
extern "C" void MH3_FrameCtrlUpdateFrm(CpuContext* ctx) {
    MH3_ANM_OBJ_CHECK("FrameCtrlUpdateFrm", func_800624B0, ControlUpdateFrmPort)
    ControlUpdateFrmPort(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x800624B0, MH3_FrameCtrlUpdateFrm, "FrameCtrlUpdateFrm", kNativeKeepsNonvolatile);

extern "C" void func_800631D4(CpuContext* ctx);
extern "C" void MH3_AnmObjChrResGetResult(CpuContext* ctx) {
    MH3_ANM_OBJ_CHECK("AnmObjChrResGetResult", func_800631D4, GetResultPort)
    GetResultPort(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x800631D4, MH3_AnmObjChrResGetResult, "AnmObjChrResGetResult", kNativeKeepsNonvolatile);

extern "C" void func_800634A0(CpuContext* ctx);
extern "C" void MH3_AnmObjChrResUpdateCache(CpuContext* ctx) {
    MH3_ANM_OBJ_CHECK("AnmObjChrResUpdateCache", func_800634A0, UpdateCachePort)
    UpdateCachePort(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x800634A0, MH3_AnmObjChrResUpdateCache, "AnmObjChrResUpdateCache", kNativeKeepsNonvolatile);
