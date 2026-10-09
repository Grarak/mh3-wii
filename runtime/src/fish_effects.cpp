// Fish, RMHP08: the effect objects of 0x8033FF88 (kind 0x22 baits, the fish
// that swim around and take them), stepped once a step by the effect object
// update. Their timers, speeds and scale changes are tick sites
// (tick_sites.inc, "fish"); three functions are decompiled from the
// disassembly:
//   0x8034027C (&angle, &target, rate) FishTurnToward: the target masked to
//     u16 and stored back, then the angle moved toward it by at most rate
//     (the short way round). The swim (rate +0x44), the bait states
//     (0x80340D14: 0x100 each step, 0x800 while hooked, 0xFFFF to face the
//     bait at once) and the line (0x803419F4, 0x800) call it once a step.
//   0x803408B4 (fish) FishSwim: state 1 (+6). Substate 0 (+7) turns toward a
//     goal (data +0x40) until the countdown (+0xC) runs out or the heading
//     (data +0x50) reaches it; substate 1 swims at a speed (data +0x20) that
//     falls by a constant each step, then rolls the next turn's length. The
//     turn rate (data +0x44) eases toward 0x20 while turning and 0x100 while
//     swimming by an eighth of the gap each step, and a wobble (data +0x48)
//     flips sign and halves each step. A bait in reach (FishFindBait) takes
//     the fish to state 2; a hunter close by or a bait of a new hunter, to
//     state 5 (flee).
//   0x8034048C (fish, mode) FishFindBait: below.
// The turn rate is per step: FishTurnToward turns by its share of the step
// (GameStep::Share), so a fish turns at the same speed at any step length; a
// rate of 0x8000 or more always reaches the target and is kept. FishSwim's
// countdown, eases and wobble run once per tick (GameStep::Ticks), its speed
// and the speed's fall by the step's length. At the stock 1/30 s step these
// are the originals (MH3_PORT_CHECK=FishSwim,FishFindBait).
#include "game_step.h"
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"
#include "ppc_runtime.h"

#include <algorithm>
#include <cstdint>

extern "C" void func_8034027C(CpuContext* ctx);
extern "C" void MH3_FishTurnToward(CpuContext* ctx) {
    const int32_t rate = int32_t(ctx->gpr[5]);

    if (rate > 0 && rate < 0x8000)
        ctx->gpr[5] = uint32_t(GameStep::Share(rate));

    func_8034027C(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x8034027C, MH3_FishTurnToward, "FishTurnToward");

namespace {

using MemoryInline::FlatRead16;
using MemoryInline::FlatRead32;
using MemoryInline::FlatRead8;
using MemoryInline::FlatReadFloat32;
using MemoryInline::FlatWrite16;
using MemoryInline::FlatWrite32;
using MemoryInline::FlatWrite8;
using MemoryInline::FlatWriteFloat32;

constexpr uint32_t kEffectObjects = 0x806A4570u; // +0xC count, +0x10 array of 0x48-byte objects
constexpr uint32_t kSpeeds = 0x805E81A0u;        // f32 by the fish's kind (+8)
constexpr uint32_t kZero = 0x8079D780u;          // 0.0f
constexpr uint32_t kSpeedScale = 0x8079D7A0u;    // a new swim's speed: this times kSpeeds[kind]
constexpr uint32_t kBaitReach = 0x8079D7A4u;     // distance within which a fish takes a bait
constexpr uint32_t kSpeedFall = 0x8079D7A8u;     // the swim speed's change per step
constexpr uint32_t kTurnScale = 0x8079D7ACu;     // the next turn's length: steps times 2k - kSpeeds[kind]

// 0x8034048C (fish, mode) FishFindBait: the bait this fish can take (an
// effect object of kind 0x22 whose +2 is not 1), or 0. The first such bait
// decides; one that is gone or past its state 1 (+0, +5) means none. The
// bait's kind for this fish (0x803402F0) goes to the fish's +8. Mode 0 (a fish
// looking for a bait) wants the bait in the water (+6 == 2), with the
// hunter's catch count (+0x597) below what the fish needs (0x803425E0),
// unless the bait is taken (+8) by a fish of kind other than 2; then, the
// hunter checked (0x8026FE38) and a kind found, the fish's heading goal (+0x40
// of its data) points at the bait. Mode 1 checks the hunter and the kind.
// Fish call it once or twice a step; most steps no bait is out, and that scan
// runs without a guest frame.
bool IsBait(uint32_t object) {
    return FlatRead8(object + 3) == 0x22 && FlatRead8(object + 2) != 1;
}

uint32_t FindBait(CpuContext* ctx, uint32_t first) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const uint32_t fish = frame.Arg(3), mode = frame.Arg(4);
    const uint32_t data = FlatRead32(fish + 0x38);
    const uint32_t objects = FlatRead32(kEffectObjects + 0x10);

    for (uint32_t i = first; int32_t(i) < int32_t(FlatRead32(kEffectObjects + 0xC)); ++i) {
        const uint32_t bait = objects + i * 0x48;

        if (!IsBait(bait))
            continue;

        if (!FlatRead8(bait) || FlatRead8(bait + 5) >= 2)
            return 0;

        const int32_t kind = int8_t(FlatRead8(data + 0x58));
        FlatWrite8(fish + 8, uint8_t(game.Call<0x803402F0>({FlatRead32(bait + 0x30), uint32_t(kind), fish})));
        const uint8_t need = uint8_t(game.Call<0x803425E0>({fish, bait}));
        const uint32_t hunter = FlatRead32(bait + 0x30);

        if (mode == 0) {
            if (FlatRead8(bait + 6) != 2)
                return 0;

            if (FlatRead8(bait + 8) && FlatRead8(fish + 8) != 2)
                continue;

            if (need <= FlatRead8(hunter + 0x597))
                return 0;
        }

        if (game.Call<0x8026FE38>({hunter}) != 1 || !FlatRead8(fish + 8)) // PlMasterCheck
            continue;

        if (mode == 0)
            FlatWrite32(data + 0x40, game.Call<0x80340248>({fish + 0x18, bait + 0x18}));

        return bait;
    }

    return 0;
}

void FishFindBaitPort(CpuContext* ctx) {
    const uint32_t objects = FlatRead32(kEffectObjects + 0x10);
    const int32_t count = int32_t(FlatRead32(kEffectObjects + 0xC));
    int32_t i = 0;

    while (i < count && !IsBait(objects + uint32_t(i) * 0x48))
        ++i;

    const uint32_t result = i < count ? FindBait(ctx, uint32_t(i)) : 0;
    ctx->gpr[3] = result;
}

// The turn rate's ease toward goal, an eighth of the gap (rounded toward
// zero) a tick.
void EaseTurnRate(uint32_t data, int32_t goal, uint32_t ticks) {
    int32_t rate = int32_t(FlatRead32(data + 0x44));

    for (uint32_t i = 0; i < ticks; ++i)
        rate += (goal - rate) / 8;

    FlatWrite32(data + 0x44, uint32_t(rate));
}

// The fish sees a hunter's bait and flees from it (state 5), remembering the
// hunter's id (data +0x5C).
void Flee(uint32_t fish) {
    FlatWrite8(fish + 6, 5);
    FlatWrite8(fish + 7, 0);
    FlatWrite8(fish + 8, 0);
}

void FishSwimPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x90);
    GuestCall game{ctx};
    const uint32_t fish = frame.Arg(3);
    const uint32_t data = FlatRead32(fish + 0x38);
    const uint32_t ticks = GameStep::Ticks();
    const double s = GameStep::Scale();
    const uint32_t bait = game.Call<0x8034048C>({fish, 0}); // FishFindBait
    bool taking = false;

    if (const uint8_t sub = FlatRead8(fish + 7); sub == 0) {
        const int32_t left = int32_t(FlatRead32(fish + 0xC));
        const int32_t next = left > 0 ? std::max(left - int32_t(ticks), 0) : left - 1;
        FlatWrite32(fish + 0xC, uint32_t(next));

        if (next <= 0 || FlatRead32(data + 0x50) == FlatRead32(data + 0x40)) {
            FlatWrite8(fish + 7, 1);
            FlatWrite8(fish + 8, 0);
            FlatWrite32(data + 0x48, 0x100);

            if (!bait) {
                const uint32_t toward = game.Call<0x80340248>({fish + 0x18, data + 8});
                const uint32_t roll = game.Call<0x800CEF20>({1}); // Rand
                FlatWrite32(data + 0x40, toward + ((roll & 0xF) << 11) - 0x4000);
            }

            FlatWriteFloat32(data + 0x20, FlatReadFloat32(kSpeedScale) * FlatReadFloat32(kSpeeds + FlatRead8(fish + 8) * 4u));
        }

        EaseTurnRate(data, 0x20, ticks);
        game.Call<0x8034027C>({data + 0x50, data + 0x40, FlatRead32(data + 0x44)}); // FishTurnToward
        FlatWrite8(data + 0x5F, 0);
    } else if (sub == 1) {
        // The step's move: (0, 0, speed) turned by the fish's angles.
        const uint32_t angles = frame[8], move = frame[32], forward = frame[44], mtx = frame[56];
        FlatWriteFloat32(forward, FlatReadFloat32(kZero));
        FlatWriteFloat32(forward + 4, FlatReadFloat32(kZero));
        FlatWriteFloat32(forward + 8, float(double(FlatReadFloat32(data + 0x20)) * s));

        for (uint32_t i = 0; i < 12; i += 4)
            FlatWrite32(angles + i, FlatRead32(data + 0x4C + i));

        game.Call<0x80050914>({angles, mtx});          // cpSetRotMatrix
        game.Call<0x80501C48>({move, mtx, forward});   // MTX34MultVecSR

        for (uint32_t i = 0; i < 12; i += 4)
            FlatWriteFloat32(fish + 0x18 + i, FlatReadFloat32(fish + 0x18 + i) + FlatReadFloat32(move + i));

        int32_t wobble = int32_t(FlatRead32(data + 0x48));

        for (uint32_t i = 0; i < ticks; ++i) {
            wobble = -wobble;
            wobble -= wobble / 2;
        }

        FlatWrite32(data + 0x48, uint32_t(wobble));
        uint32_t near = 0;

        if (bait) {
            game.Call<0x80050FE4>({fish + 0x18, bait + 0x18}); // the distance, in f1

            if (ctx->fpr[1].d < double(FlatReadFloat32(kBaitReach))) {
                FlatWrite8(bait + 8, FlatRead8(bait + 8) | 1);
                near = bait;
            }
        }

        FlatWrite32(data + 0x38, near);

        if (near) {
            if (const uint32_t hunter = FlatRead32(near + 0x30); hunter && FlatRead8(hunter + 0x596)) {
                FlatWrite8(fish + 6, 2);
                FlatWrite8(fish + 7, 0);
                FlatWrite32(fish + 0xC, 3);
                FlatWrite32(data + 0x48, 0);
                taking = true;
            }
        }

        const float speed = float(double(FlatReadFloat32(data + 0x20)) + double(FlatReadFloat32(kSpeedFall)) * s);
        FlatWriteFloat32(data + 0x20, speed);

        if (speed <= FlatReadFloat32(kZero) && !taking) {
            FlatWrite8(fish + 7, 0);
            const int32_t steps = int32_t(game.Call<0x800CEF20>({1}) & 0xFFFF) % 20 + 10; // Rand
            const float k = FlatReadFloat32(kTurnScale);
            const float scale = k - (FlatReadFloat32(kSpeeds + FlatRead8(fish + 8) * 4u) - k);
            FlatWrite32(fish + 0xC, uint32_t(int32_t(float(steps) * scale)));
            FlatWrite32(data + 0x48, 0);
        }

        EaseTurnRate(data, 0x100, ticks);
        game.Call<0x8034027C>({data + 0x50, data + 0x40, FlatRead32(data + 0x44)}); // FishTurnToward
    }

    // A hunter close by (in its hit sphere, data +0x30 around data +8) who is
    // in the water or moving.
    const uint32_t hunter = game.Call<0x800CFAF4>({2}) + uint32_t(int32_t(int8_t(game.Call<0x800CF3E8>()))) * 0xB20; // get_move_work_adrs, MyPlayerIndex

    if (game.Call<0x803405F8>({hunter}) == 0 && (FlatRead8(hunter + 9) == 3 || (FlatRead16(hunter + 0x5A4) & 2)) &&
        game.Call<0x8028F680>({hunter + 0x3C, data + 8}, {double(FlatReadFloat32(data + 0x30))}) == 1) { // HitPointSphere
        Flee(fish);
        FlatWrite8(data + 0x5B, 0);
        FlatWrite16(data + 0x5C, 0xFFFF);
    }

    if (FlatRead8(data + 0x60) == 1 && bait) {
        const uint16_t id = FlatRead16(FlatRead32(bait + 0x30) + 0x306);

        if (FlatRead16(data + 0x5C) != id) {
            Flee(fish);
            FlatWrite8(data + 0x5B, 1);
            FlatWrite16(data + 0x5C, id);
        }
    }

    const uint32_t seen = game.Call<0x8034048C>({fish, 1}); // FishFindBait

    if (FlatRead8(fish + 8) == 3) {
        Flee(fish);

        if (seen)
            FlatWrite16(data + 0x5C, FlatRead16(FlatRead32(seen + 0x30) + 0x306));

        FlatWrite8(data + 0x5B, 1);
    }
}

} // namespace

extern "C" void func_8034048C(CpuContext* ctx);
extern "C" void MH3_FishFindBait(CpuContext* ctx) {
    PortCheck::Call(ctx, "FishFindBait", func_8034048C, FishFindBaitPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8034048C, MH3_FishFindBait, "FishFindBait");

extern "C" void func_803408B4(CpuContext* ctx);
extern "C" void MH3_FishSwim(CpuContext* ctx) {
    PortCheck::Call(ctx, "FishSwim", func_803408B4, FishSwimPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803408B4, MH3_FishSwim, "FishSwim");
