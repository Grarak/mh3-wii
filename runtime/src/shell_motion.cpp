// Shells, objects and enemies moved by velocity, and the player's status
// tint, RMHP08: per-step motion scaled by the step length.
//
// Shells (_SHELL_W: projectiles, thrown and rolling objects; the quest_data
// RSO runs most of them) move once a step through two helpers, decompiled
// from the disassembly:
//   shell_rate_add   (0x802AB3A8): position (+0x18) += velocity (+0x3C)
//   shell_rate_add_g (0x802AB3B8): the same, then velocity += acceleration (+0x48)
// Here both amounts are taken over this step's length s (GameStep::Scale), so
// shells fly and fall at the same speed at any step length. At the stock
// 1/30 s step (s = 1) this is the original.
//
// Enemies and other objects move the same way through VecAddInPlace
// (0x80073FCC: *r3 += *r4 through PSVECAdd, returns r3), called once a step
// with position += velocity or velocity += acceleration from the places in
// kRateCalls (by return address): the enemy helpers 0x8013548C (+0x188 +=
// +0x310, +0x310 += +0x31C; 0x8013547C, the position add alone, is its own
// native below) and 0x80135558 (+0x188 += +0x310 rotated by the
// heading, for flight), the monsters that call it inline (0x801A0CAC,
// a desert enemy's swim, ...), the camera demo actor, the ambient creatures,
// falling debris, the desert object group. For those the addend is taken over this step's length; the
// other 200-odd callers (offsets, collision push-outs, one-off sums) add it
// whole. The desert enemy also takes the distance it moved, the length of
// its velocity (VecMagWrapper 0x80050F88, through PSVECMag), off a budget
// (+0x378) each step: at those three calls the length is scaled likewise.
//
// The player's status tint (0x802E77EC, per player from 0x802E79D0) advances
// two u16 phases (+0x13E, +0x140) once a step: by 960, 1456, 512 or 768, or a
// rate from a table (0x805C6434) through three helpers, and sets them to 0
// when the status ends. The wrapper lets the original run, then moves each
// phase on by its share of the advance, GameStep::Share. The colours the
// original derived from the phase this step lead by the rest of one step's
// advance (a 1/30 s lead at most); the phases themselves keep game time.
#include "game_step.h"
#include "memory.h"
#include "port_check.h"
#include "ppc_runtime.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>

namespace {

constexpr uint32_t kPosition = 0x18, kVelocity = 0x3C, kAcceleration = 0x48; // f32 x, y, z

// v += d * s for the f32 vectors at v and d.
void AddScaled(uint32_t v, uint32_t d, float s) {
    for (uint32_t i = 0; i < 12; i += 4)
        MemoryInline::FlatWriteFloat32(v + i, MemoryInline::FlatReadFloat32(v + i) + MemoryInline::FlatReadFloat32(d + i) * s);
}

void ShellRateAddPort(CpuContext* ctx) {
    const uint32_t shell = ctx->gpr[3];
    AddScaled(shell + kPosition, shell + kVelocity, float(GameStep::Scale()));
}

void ShellRateAddGPort(CpuContext* ctx) {
    const uint32_t shell = ctx->gpr[3];
    const float s = float(GameStep::Scale());
    AddScaled(shell + kPosition, shell + kVelocity, s);
    AddScaled(shell + kVelocity, shell + kAcceleration, s);
}

// VecAddInPlace calls that move by a rate, by return address (the bl + 4).
constexpr uint32_t kRateCalls[] = {
    0x801354ACu, 0x801354B8u,                           // 0x8013548C enemy: position += velocity, velocity += acceleration
    0x801355D0u,                                        // 0x80135558 enemy: position += rotated velocity (flying)
    0x80135650u,                                        // 0x8013562C enemy: velocity += acceleration
    0x80135C08u, 0x80135C14u,                           // 0x80135BE8: +0x14 += +0x5C, +0x5C += +0x68
    0x801A07DCu, 0x801A0AE4u, 0x801A0EBCu,              // desert enemy swims: position += velocity
    0x801BA68Cu, 0x801BA6C4u, 0x801BA950u, 0x801BA988u, // 0x801BA438: +0x34C += velocity, velocity += acceleration
    0x801CC344u, 0x801CC368u,                           // 0x801CBD94: position += velocity
    0x801CF5D4u, 0x801CF614u,                           // 0x801CF124: position += velocity
    0x801D2674u, 0x801D2698u,                           // 0x801D2208: position += velocity
    0x802C35C4u, 0x802C35D0u,                           // 0x802C3444: +0x178 += +0x1D4, +0x1D4 += +0x1E0
    0x802D3304u,                                        // 0x802D32E0: +0x1D4 += +0x1E0
    0x802D8578u,                                        // 0x802D853C camera demo actor: +0x178 += +0x470
    0x802F513Cu,                                        // 0x802F511C: position += velocity
    0x80308A18u,                                        // 0x80308960 debris: position += velocity
    0x8031E48Cu,                                        // 0x8031DF1C: +0x14 += +0x20
    0x80328E2Cu,                                        // 0x80328BDC enemy: velocity += acceleration
    0x80369404u, 0x80369818u, 0x80369C54u,              // 0x803692F0 desert object group: position += velocity
};

bool IsRateCall(uint32_t lr) {
    return std::binary_search(std::begin(kRateCalls), std::end(kRateCalls), lr);
}

void VecAddInPlace(uint32_t lr, uint32_t out, uint32_t add) {
    const double s = GameStep::Scale();
    AddScaled(out, add, s != 1.0 && IsRateCall(lr) ? float(s) : 1.0f);
}

void VecAddInPlacePort(CpuContext* ctx) {
    VecAddInPlace(ctx->lr, ctx->gpr[3], ctx->gpr[4]);
}

// VecMagWrapper calls that measure a step's move, by return address.
constexpr uint32_t kStepLengthCalls[] = {0x801A07E4u, 0x801A0AECu, 0x801A0EC4u}; // then +0x378 -= it

// PSVECMag: sqrt(x*x + y*y + z*z) as the paired-single code computes it (the
// same float helpers as its translation), the frsqrte estimate refined by one
// Newton step; 0 for a zero vector.
float VecMag(uint32_t v) {
    const double xy = PPC_PsMulInline(PpcPackPairedInline(MemoryInline::FlatReadFloat32(v), MemoryInline::FlatReadFloat32(v + 4)),
                                      PpcPackPairedInline(MemoryInline::FlatReadFloat32(v), MemoryInline::FlatReadFloat32(v + 4)));
    const double z = PPC_PsFromScalarInline(MemoryInline::FlatReadFloat32(v + 8));
    const double sq = PPC_PsToScalarInline(PPC_PsSum0Inline(PPC_PsMaddInline(z, z, xy), xy, xy));

    if (sq == 0.0)
        return float(sq);

    const double e = PPC_Frsqrte(sq);
    const double refine = PPC_Fnmsubs(PpcFmulsInline(e, e), sq, 3.0); // 3 - e*e*sq
    return float(PpcFmulsInline(sq, PpcFmulsInline(refine, PpcFmulsInline(e, 0.5))));
}

float VecMagWrapper(uint32_t lr, uint32_t v) {
    const float mag = VecMag(v);
    const double s = GameStep::Scale();

    if (s != 1.0 && std::find(std::begin(kStepLengthCalls), std::end(kStepLengthCalls), lr) != std::end(kStepLengthCalls))
        return float(mag * s);

    return mag;
}

void VecMagWrapperPort(CpuContext* ctx) {
    ctx->fpr[1].d = VecMagWrapper(ctx->lr, ctx->gpr[3]);
}

constexpr uint32_t kTintPhases[] = {0x13E, 0x140}; // u16

} // namespace

extern "C" void func_802AB3A8(CpuContext* ctx);
extern "C" void MH3_ShellRateAdd(CpuContext* ctx) {
    PortCheck::Call(ctx, "ShellRateAdd", func_802AB3A8, ShellRateAddPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x802AB3A8, MH3_ShellRateAdd, "ShellRateAdd");

extern "C" void func_802AB3B8(CpuContext* ctx);
extern "C" void MH3_ShellRateAddG(CpuContext* ctx) {
    PortCheck::Call(ctx, "ShellRateAddG", func_802AB3B8, ShellRateAddGPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x802AB3B8, MH3_ShellRateAddG, "ShellRateAddG");

extern "C" void func_80073FCC(CpuContext* ctx);

// The registered body names only r1, r3, r4 and lr, so callers keep a precise
// call boundary; MH3_PORT_CHECK=VecAddInPlace checks the written vector.
static void VecAddInPlaceChecked([[maybe_unused]] uint32_t r1, uint32_t lr, uint32_t out, uint32_t add) {
#ifdef MH3_DEVELOPMENT
    const bool check = PortCheck::Enabled("VecAddInPlace");

    if (check) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[1] = r1;
        c->gpr[3] = out;
        c->gpr[4] = add;
        c->lr = lr;
        PortCheck::RunOutput(c, "VecAddInPlace", func_80073FCC, VecAddInPlacePort, {{out, 12}});
        return;
    }
#endif
    VecAddInPlace(lr, out, add);
}

extern "C" void MH3_VecAddInPlace(CpuContext* ctx) {
    VecAddInPlaceChecked(ctx->gpr[1], ctx->lr, ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80073FCC, MH3_VecAddInPlace, "VecAddInPlace");

// 0x8013547C: enemy position (+0x188) += velocity (+0x310), a tail call of
// VecAddInPlace from its callers (whose return addresses it then sees): the
// move of every enemy that calls it, so it always takes the step's share.
constexpr uint32_t kEnemyPosition = 0x188, kEnemyVelocity = 0x310;
static void EnemyAddVelocityPort(CpuContext* ctx) {
    const uint32_t enemy = ctx->gpr[3];
    AddScaled(enemy + kEnemyPosition, enemy + kEnemyVelocity, float(GameStep::Scale()));
    ctx->gpr[3] = enemy + kEnemyPosition;
    ctx->gpr[4] = enemy + kEnemyVelocity;
}

extern "C" void func_8013547C(CpuContext* ctx);
extern "C" void MH3_EnemyAddVelocity(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    const bool check = PortCheck::Enabled("EnemyAddVelocity");

    if (check) [[unlikely]] {
        PortCheck::RunOutput(ctx, "EnemyAddVelocity", func_8013547C, EnemyAddVelocityPort, {{ctx->gpr[3] + kEnemyPosition, 12}});
        return;
    }
#endif
    EnemyAddVelocityPort(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x8013547C, MH3_EnemyAddVelocity, "EnemyAddVelocity");

extern "C" void func_80050F88(CpuContext* ctx);
static double VecMagWrapperChecked(uint32_t lr, uint32_t v) {
#ifdef MH3_DEVELOPMENT
    const bool check = PortCheck::Enabled("VecMagWrapper");

    if (check) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[3] = v;
        c->lr = lr;
        PortCheck::RunPure(c, "VecMagWrapper", PortCheck::Returns::Fpr1, func_80050F88, VecMagWrapperPort);
        return c->fpr[1].d;
    }
#endif
    return VecMagWrapper(lr, v);
}

extern "C" void MH3_VecMagWrapper(CpuContext* ctx) {
    ctx->fpr[1].d = VecMagWrapperChecked(ctx->lr, ctx->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80050F88, MH3_VecMagWrapper, "VecMagWrapper");

extern "C" void func_802E77EC(CpuContext* ctx);
extern "C" void MH3_PlayerTintPulse(CpuContext* ctx) {
    const uint32_t work = ctx->gpr[3];
    uint16_t before[2];

    for (int i = 0; i < 2; ++i)
        before[i] = Memory::Read16(work + kTintPhases[i]);

    func_802E77EC(ctx);

    if (GameStep::Scale() == 1.0)
        return;

    for (int i = 0; i < 2; ++i) {
        const uint16_t after = Memory::Read16(work + kTintPhases[i]);

        if (after == 0 || after == before[i])
            continue; // ended, or not advancing
        const int32_t advance = uint16_t(after - before[i]);
        Memory::Write16(work + kTintPhases[i], uint16_t(before[i] + GameStep::Share(advance)));
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x802E77EC, MH3_PlayerTintPulse, "PlayerTintPulse");
