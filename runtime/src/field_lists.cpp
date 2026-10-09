// Per-frame updates of the field's object lists and small geometry helpers,
// RMHP08. C reconstructions from the disassembly:
//
//   0x800F8E08 EffectObjectsUpdate(): the effect object manager at
//              0x806A4570 (+4 the manager, +0xC count and +0x10 array of
//              0x48-byte entries, update at +0x34): the manager's step
//              (0x800F9608), the live entries' updates, then the manager's
//              owner's virtual +0xC; byte 0x806A51B0 cleared
//   0x800D854C ShellSlotsUpdate(obj): the object's 32 shell slots (0x50
//              bytes from +0x3C; u8 live +0, kind +1, state +2): kinds 3 and
//              4 advance (0x800D8F0C) in states 2 (kind 3 only), 3 and 4,
//              then ShellSlotUpdate (0x800D9348) and ShellSlotFinish
//              (0x800D8468)
//   0x800D8614 ShellSlotsUpdate2(obj, ...): the same (the 3D sources' kinds
//              9-12)
//   0x800D8794 ShellSlotsRefresh(obj): kinds 3 and 4 only updated and
//              finished
//   0x8028F5BC CapsuleSetup(seg, out): a hit capsule from a segment
//              {p0, p1, radius}: out = {p0, p1, radius, p1 - p0, the middle,
//              the bounding radius |middle - p0| + radius}
//   0x80099418 ResOffsetOrNull(ref, offset): *ref + offset, 0 for offset 0
//   0x802AAE40 FieldObjectsUpdate(u8 phase): the field objects (0x10C bytes
//              each, array *(0x806AFB20), count *(0x806AFB1C)) of that phase
//              (u8 +0x108): each live one's update (+0xF8), then its two
//              effect holders (+0x5C 0x8029F040, +0xB8 0x8029F070)
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"
#include "ps_vec_math.h"

#include <cstdint>

namespace {

uint32_t Read32(uint32_t a) {
    return MemoryInline::FlatRead32(a);
}

float ReadFloat(uint32_t a) {
    return MemoryInline::FlatReadFloat32(a);
}

void WriteFloat(uint32_t a, float v) {
    MemoryInline::FlatWriteFloat32(a, v);
}

constexpr uint32_t kEffectObjects = 0x806A4570u;

void EffectObjectsUpdate(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const uint32_t manager = Read32(kEffectObjects + 4);

    if (!manager)
        return;

    game.Call<0x800F9608>({manager});
    MemoryInline::FlatWrite8(kEffectObjects + 0xC40, 0);

    for (int32_t i = 0; i < int32_t(Read32(kEffectObjects + 0xC)); ++i) {
        const uint32_t entry = Read32(kEffectObjects + 0x10) + uint32_t(i) * 0x48;

        if (MemoryInline::FlatRead8(entry))
            game(Read32(entry + 0x34), {entry});
    }

    game.Virtual<0xC>(Read32(manager));
}

// The 32 shell slots: kinds 3 and 4 advanced (when `advance`), updated and
// finished.
void ShellSlots(CpuContext* ctx, bool advance) {
    GuestFrame frame(ctx, 0x30);
    GuestCall game{ctx};
    const uint32_t obj = frame.Arg(3);

    for (uint32_t i = 0; i < 32; ++i) {
        const uint32_t slot = obj + 0x3C + i * 0x50;

        if (!MemoryInline::FlatRead8(slot))
            continue;

        const uint32_t kind = MemoryInline::FlatRead8(slot + 1);

        if (kind != 3 && kind != 4)
            continue;

        if (advance) {
            if (kind == 3 && MemoryInline::FlatRead8(slot + 2) == 2)
                game.Call<0x800D8F0C>({obj, slot});

            if (uint8_t(MemoryInline::FlatRead8(slot + 2) + 253) <= 1) // state 3 or 4
                game.Call<0x800D8F0C>({obj, slot});
        }

        game.Call<0x800D9348>({obj, slot}); // ShellSlotUpdate
        game.Call<0x800D8468>({slot});      // ShellSlotFinish
    }
}

void ShellSlotsUpdate(CpuContext* ctx) {
    ShellSlots(ctx, true);
}

void ShellSlotsRefresh(CpuContext* ctx) {
    ShellSlots(ctx, false);
}

void FieldObjectsUpdate(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const uint32_t phase = frame.Arg(3) & 0xFF;

    for (int32_t i = 0; i < int32_t(Read32(0x806AFB1Cu)); ++i) {
        const uint32_t object = Read32(0x806AFB20u) + uint32_t(i) * 0x10C;

        if (!MemoryInline::FlatRead8(object) || MemoryInline::FlatRead8(object + 0x108) != phase)
            continue;

        game(Read32(object + 0xF8), {object});
        game.Call<0x8029F040>({object + 0x5C});
        game.Call<0x8029F070>({object + 0xB8});
    }
}

void CapsuleSetup(uint32_t seg, uint32_t out) {
    for (uint32_t k = 0; k < 0x1C; k += 4)
        MemoryInline::FlatWrite32(out + k, Read32(seg + k)); // p0, p1, radius

    const float half = ReadFloat(0x8079C7D0u); // 0.5f (r2 - 14224)

    for (uint32_t k = 0; k < 12; k += 4) {
        WriteFloat(out + 0x1C + k, ReadFloat(seg + 0xC + k) - ReadFloat(seg + k));
        WriteFloat(out + 0x28 + k, half * (ReadFloat(seg + k) + ReadFloat(seg + 0xC + k)));
    }

    WriteFloat(out + 0x34, PsVec::Distance(seg, out + 0x28) + ReadFloat(seg + 0x18));
}

uint32_t OffsetOrNull(uint32_t ref, uint32_t offset) {
    return offset ? Read32(ref) + offset : 0;
}

} // namespace

extern "C" void func_800F8E08(CpuContext* ctx);
extern "C" void MH3_EffectObjectsUpdate(CpuContext* ctx) {
    PortCheck::Call(ctx, "EffectObjectsUpdate", func_800F8E08, EffectObjectsUpdate);
}
REGISTER_NATIVE_FUNCTION_AS(0x800F8E08, MH3_EffectObjectsUpdate, "EffectObjectsUpdate", kNativeKeepsNonvolatile);

extern "C" void func_800D854C(CpuContext* ctx);
extern "C" void MH3_ShellSlotsUpdate(CpuContext* ctx) {
    PortCheck::Call(ctx, "ShellSlotsUpdate", func_800D854C, ShellSlotsUpdate);
}
REGISTER_NATIVE_FUNCTION_AS(0x800D854C, MH3_ShellSlotsUpdate, "ShellSlotsUpdate", kNativeKeepsNonvolatile);

extern "C" void func_800D8614(CpuContext* ctx);
extern "C" void MH3_ShellSlotsUpdate2(CpuContext* ctx) {
    PortCheck::Call(ctx, "ShellSlotsUpdate2", func_800D8614, ShellSlotsUpdate);
}
REGISTER_NATIVE_FUNCTION_AS(0x800D8614, MH3_ShellSlotsUpdate2, "ShellSlotsUpdate2", kNativeKeepsNonvolatile);

extern "C" void func_800D8794(CpuContext* ctx);
extern "C" void MH3_ShellSlotsRefresh(CpuContext* ctx) {
    PortCheck::Call(ctx, "ShellSlotsRefresh", func_800D8794, ShellSlotsRefresh);
}
REGISTER_NATIVE_FUNCTION_AS(0x800D8794, MH3_ShellSlotsRefresh, "ShellSlotsRefresh", kNativeKeepsNonvolatile);

extern "C" void func_802AAE40(CpuContext* ctx);
extern "C" void MH3_FieldObjectsUpdate(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldObjectsUpdate", func_802AAE40, FieldObjectsUpdate);
}
REGISTER_NATIVE_FUNCTION_AS(0x802AAE40, MH3_FieldObjectsUpdate, "FieldObjectsUpdate", kNativeKeepsNonvolatile);

extern "C" void func_8028F5BC(CpuContext* ctx);
static void CapsuleSetupPort(CpuContext* p) {
    CapsuleSetup(p->gpr[3], p->gpr[4]);
}

static void CapsuleSetupChecked([[maybe_unused]] uint32_t r1, uint32_t r3, uint32_t r4) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Enabled("CapsuleSetup")) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[1] = r1;
        c->gpr[2] = 0x8079FF60u;
        c->gpr[3] = r3;
        c->gpr[4] = r4;
        PortCheck::RunOutput(c, "CapsuleSetup", func_8028F5BC, CapsuleSetupPort, {{r4, 0x38}});
        return;
    }
#endif
    CapsuleSetup(r3, r4);
}

extern "C" void MH3_CapsuleSetup(CpuContext* ctx) {
    CapsuleSetupChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8028F5BC, MH3_CapsuleSetup, "CapsuleSetup");

extern "C" void func_80099418(CpuContext* ctx);
static void OffsetOrNullPort(CpuContext* p) {
    p->gpr[3] = OffsetOrNull(p->gpr[3], p->gpr[4]);
}

static uint32_t OffsetOrNullChecked([[maybe_unused]] uint32_t r1, uint32_t r3, uint32_t r4) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Enabled("ResOffsetOrNull")) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[1] = r1;
        c->gpr[3] = r3;
        c->gpr[4] = r4;
        PortCheck::RunPure(c, "ResOffsetOrNull", PortCheck::Returns::Gpr3, func_80099418, OffsetOrNullPort);
        return c->gpr[3];
    }
#endif
    return OffsetOrNull(r3, r4);
}

extern "C" void MH3_ResOffsetOrNull(CpuContext* ctx) {
    ctx->gpr[3] = OffsetOrNullChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80099418, MH3_ResOffsetOrNull, "ResOffsetOrNull");
