// Player turning, RMHP08. C reconstructions of the player's per-step turn
// toward the stick direction: 0x80260304 in the field and 0x8020A258 in the
// village (called from the player updates 0x80209xxx / 0x8025xxxx each step),
// decompiled with Ghidra.
//
// Each step the yaw (PLW+0x58) turns toward the wanted yaw (+0xA8) by a fixed
// rate (4004, 1820, 1456, ... angle units, 0x10000 a turn; it snaps once
// closer than the rate), the body leans into the turn (+0xB0, by 128 or 160
// per step, back by 768 per step once facing, within +-2560), and in the field
// the pitch (+0x54) turns toward +0xAC and a turn blend (+0x648, +-10 or 5 per
// step within +-100) drives a pair of motion channels. Every one of those
// amounts is per step: here each is GameStep::Share of it, the amount scaled
// by the step length, so the player turns at the Wii's rate at any step
// length. At the stock 1/30 s step every share is the amount: the original.
#include "game_step.h"
#include "guest_call.h"
#include "port_check.h"

#include <cstdint>

namespace {

enum : uint32_t {
    MasterCheck = 0x8026FE38,      // Pl_master_ck(PLW*) -> 1 for the local player
    ActionCheck = 0x8026FECC,      // Pl_act_ck(PLW*, u8 kind, u16 action) -> 1
    ActionFlagCheck = 0x8026FEFC,  // (PLW*, flags) -> nonzero
    TurnLocked = 0x8027A3A4,       // (PLW*) -> 1: pitch holds
    MotionNo = 0x8026A36C,         // Get_motion_no(PLW*) -> u16
    AddTurnBlend = 0x802602AC,     // (PLW*, mode, s16 amount): +0x648, within +-100
    BlendFrame = 0x8026A3B0,       // (PLW*; f1 blend) -> f1 frame
    SetChannelMotion = 0x8026A294, // (PLW*, channel, motion, 0, s16 frame)
    SetChannelWeight = 0x8026A2A0, // (PLW*, channel; f1 weight)
};

enum : uint32_t {
    kAction = 0x09,    // u8
    kState = 0x0A,     // u8
    kSubState = 0x0C,  // u16
    kWeaponOut = 0x18, // u8
    kPitch = 0x54,     // u32 (u16 angle)
    kYaw = 0x58,       // u32 (u16 angle)
    kWantedPitch = 0xAC,
    kWantedYaw = 0xA8,
    kLean = 0xB0,       // u16 (s16 angle)
    kTurnBlend = 0x648, // s16, -100..100
};

constexpr uint32_t kOne = 0x8079C2C8;          // f32 1.0
constexpr uint32_t kZero = 0x8079C2C0;         // f32 0.0
constexpr uint32_t kBlendPerUnit = 0x8079C48C; // f32 0.01
constexpr uint32_t kMinusOne = 0x8079C310;     // f32 -1.0

struct Plw {
    uint32_t base;
    uint8_t U8(uint32_t o) const { return Memory::Read8(base + o); }
    uint16_t U16(uint32_t o) const { return Memory::Read16(base + o); }
    uint32_t U32(uint32_t o) const { return Memory::Read32(base + o); }
    void W16(uint32_t o, uint32_t v) const { Memory::Write16(base + o, uint16_t(v)); }
    void W32(uint32_t o, uint32_t v) const { Memory::Write32(base + o, v); }
};

// Turns the u16 angle at `angle` toward `wanted` by `rate`; true when it
// arrived (snapped). The original compares with the full per-step rate.
bool TurnToward(const Plw& p, uint32_t angle, uint32_t wanted, uint32_t rate, uint32_t* difference = nullptr) {
    const uint32_t diff = (p.U32(wanted) - p.U32(angle)) & 0xFFFF;

    if (difference)
        *difference = diff;

    if (((diff + rate) & 0xFFFF) < rate * 2) {
        p.W32(angle, p.U32(wanted));
        return true;
    }

    p.W32(angle, (diff < 0x8000 ? p.U32(angle) + rate : p.U32(angle) - rate) & 0xFFFF);
    return false;
}

// The lean settles back by `back` per step once facing (as the original: a
// small positive lean ends at 0, a negative one wraps past it).
void LeanBack(const Plw& p, uint32_t back) {
    const uint32_t lean = p.U16(kLean);

    if (int32_t(lean + back) <= int32_t(back * 2))
        p.W16(kLean, 0);
    else if (lean < 0x8000)
        p.W16(kLean, lean - back);
    else
        p.W16(kLean, lean + back);
}

void ClampLean(const Plw& p) {
    const uint32_t lean = p.U16(kLean);

    if (((lean + 0x10000 - 0xA00) & 0xFFFF) <= 0xEC00)
        p.W16(kLean, lean < 0x8000 ? 0xA00 : 0xF600);
}

uint32_t Share(int32_t amount) {
    return uint32_t(GameStep::Share(amount));
}

} // namespace

extern "C" void func_80260304(CpuContext* ctx);
extern "C" void func_8020A258(CpuContext* ctx);

static void PlayerTurnPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x40);
    GuestCall game{ctx};
    const Plw p{frame.Arg(3)};
    const uint32_t savedR30 = frame.Arg(30);
    auto act = [&](uint32_t kind, uint32_t action) {
        return game.Call<ActionCheck>({p.base, kind, action}) == 1;
    };

    int32_t yawRate = 4004, pitchRate = 1820;

    if (p.U8(kWeaponOut) == 1 && p.U8(kState) == 1 && p.U16(kSubState) <= 1) {
        yawRate = 2548;
    } else if (act(1, 1) || act(1, 5)) {
        yawRate = 1820;
    } else if (act(1, 2) || act(1, 14)) {
        yawRate = 1456;
    } else if (p.U8(kAction) == 3) {
        if (game.Call<ActionFlagCheck>({p.base, 192}) != 0) {
            yawRate = p.U8(kWeaponOut) == 1 ? 1820 : 1092;
            pitchRate = 364;
        } else {
            yawRate = 1820;
        }
    } else if (act(1, 4)) {
        yawRate = 0;
    }

    if (game.Call<MasterCheck>({p.base}) == 1 && act(0, 22) && game.Call<TurnLocked>({p.base}) == 1)
        pitchRate = 0;

    const uint32_t motion = game.Call<MotionNo>({p.base}) & 0xFFFF;
    const int32_t lean = motion == 3 ? 128 : motion == 4 ? 160 : 0;
    auto blend = [&](uint32_t mode, int32_t amount) {
        game.Call<AddTurnBlend>({p.base, mode, Share(amount)});
    };

    uint32_t diff = 0;

    if (TurnToward(p, kYaw, kWantedYaw, Share(yawRate), &diff)) {
        LeanBack(p, Share(768));

        if (((diff + 256) & 0xFFFF) < 512) {
            const int16_t turn = int16_t(p.U16(kTurnBlend));

            if (turn > 0)
                blend(1, -5);
            else if (turn < 0)
                blend(1, 5);
        } else {
            blend(0, diff < 0x8000 ? -10 : 10);
        }
    } else if (diff < 0x8000) {
        p.W16(kLean, p.U16(kLean) - Share(lean));
        blend(0, -10);
    } else {
        p.W16(kLean, p.U16(kLean) + Share(lean));
        blend(0, 10);
    }

    ClampLean(p);
    TurnToward(p, kPitch, kWantedPitch, Share(pitchRate));

    // Turning in place: the turn blend weights a pair of motion channels.
    if (!(motion - 119 < 2 || motion == 101 || motion == 115))
        return;

    const int16_t turn = int16_t(p.U16(kTurnBlend));

    if (turn == 0) {
        game.Call<SetChannelWeight>({p.base, 0}, {Memory::ReadFloat32(kOne)});
        game.Call<SetChannelWeight>({p.base, 1}, {Memory::ReadFloat32(kZero)});
        return;
    }

    const float amount = float(turn);
    float weight = Memory::ReadFloat32(kBlendPerUnit) * amount;
    uint32_t turnMotion = savedR30;

    if (turn > 0) {
        switch (motion) {
        case 101:
            turnMotion = 111;
            break;
        case 115:
            turnMotion = 127;
            break;
        case 119:
            turnMotion = 133;
            break;
        case 120:
            turnMotion = 129;
            break;
        }
    } else {
        switch (motion) {
        case 101:
            turnMotion = 125;
            break;
        case 115:
            turnMotion = 128;
            break;
        case 119:
            turnMotion = 134;
            break;
        case 120:
            turnMotion = 130;
            break;
        }

        weight *= Memory::ReadFloat32(kMinusOne);
    }

    game.Call<BlendFrame>({p.base}, {amount});
    const int16_t blendFrame = int16_t(int32_t(game.F1()));
    game.Call<SetChannelMotion>({p.base, 1, turnMotion, 0, uint32_t(int32_t(blendFrame))});
    game.Call<SetChannelWeight>({p.base, 0}, {Memory::ReadFloat32(kOne) - weight});
    game.Call<SetChannelWeight>({p.base, 1}, {weight});
}

static void VillagePlayerTurnPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const Plw p{frame.Arg(3)};

    if (game.Call<MasterCheck>({p.base}) == 0) {
        p.W16(kLean, 0);
        return;
    }

    int32_t rate = 4004;
    const uint16_t sub = p.U16(kSubState);

    if (p.U8(kState) == 1)
        rate = sub == 1 || sub == 4 ? 1820 : sub == 2 ? 1456 : 4004;
    else if (p.U8(kState) == 12)
        rate = sub == 2 || sub == 5 ? 1820 : sub == 3 ? 1456 : 4004;

    const uint32_t motion = game.Call<MotionNo>({p.base}) & 0xFFFF;
    const int32_t lean = motion == 3 || motion == 53 ? 128 : motion == 4 ? 160 : 0;
    uint32_t diff = 0;

    if (TurnToward(p, kYaw, kWantedYaw, Share(rate), &diff))
        LeanBack(p, Share(768));
    else if (diff < 0x8000)
        p.W16(kLean, p.U16(kLean) - Share(lean));
    else
        p.W16(kLean, p.U16(kLean) + Share(lean));

    ClampLean(p);
}

extern "C" void MH3_PlayerTurn(CpuContext* ctx) {
    PortCheck::Call(ctx, "PlayerTurn", func_80260304, PlayerTurnPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80260304, MH3_PlayerTurn, "PlayerTurn");

extern "C" void MH3_VillagePlayerTurn(CpuContext* ctx) {
    PortCheck::Call(ctx, "VillagePlayerTurn", func_8020A258, VillagePlayerTurnPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8020A258, MH3_VillagePlayerTurn, "VillagePlayerTurn");
