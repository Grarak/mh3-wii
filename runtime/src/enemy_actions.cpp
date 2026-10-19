// Enemy actions, RMHP08: how a monster starts, picks and ends its actions.
// C reconstructions (decompiled with Ghidra, checked against the
// disassembly):
//
//   0x80128624 EnemySetAction (work, mode, sub, how): starts an action
//   0x80126520 EnemyHandler (work, index): the species' handler `index`
//   0x8013C4BC EnemyAiPick (work, pair*): the AI's pick
//   0x80127FAC EnemyActionDone (work): an action's end, back to waiting
//   0x80128B10 EnemyBackTo (work, mode, sub): a waiting action (how 2)
//   0x801D8F00 Em007TailSpin (work): Deviljho's tail spin, action 7.1
//
// A monster's action is the pair mode (+0x1E5) and sub (+0x1E6); its routine
// state is +5..+7, cleared when it starts. `how` says who starts it: 0 the
// AI (online, also sent to the other players), 1 a spawn, 2 an action's end,
// 3 and 4 the network's copies (they keep the random word +0x1EC), 5 a
// follow-up of the action running. The monster control (development builds,
// monster_control.cpp) traces every start and can drop the AI's picks.
#include "abi_bridge.h"
#include "enemy_work.h"
#include "guest_call.h"
#include "memory.h"
#include "monster_control.h"
#include "port_check.h"

#include <cstdint>

namespace {

enum : uint32_t {
    kKind = 0x003,       // u8: the enemy kind
    kRoutine = 0x005,    // u8[3]: the action's routine state
    kPlace = 0x116,      // u8
    kFlags = 0x1C8,      // u32: 8 the AI runs here (the host online), 0x40 ...
    kYaw = 0x1C0,        // s32: 65536 a turn
    kMedium = 0x1E2,     // u8: 2 in water (EnemyPartEnabled), 1, 3, 4 other places
    kMediumSub = 0x1E3,  // u8
    kMode = 0x1E5,       // u8: the action's mode (11: dying or captured)
    kSub = 0x1E6,        // u8: the action within the mode
    kStarted = 0x1E8,    // u8: 1 after an action's end (how 2), else 0
    kPending = 0x1E9,    // u8: an action asked for while it could not act
    kSeed = 0x1EC,       // u16: the action's random word
    kSentMode = 0x1EE,   // u8, u8, u16: the action as sent online
    kHunting = 0x1F5,    // u8: 1 while it may act (0x8012D204)
    kTurnFlags = 0x1FB,  // u8
    kTarget = 0x38C,     // u8
    kWaitSub = 0x43D,    // u8: 1 waits in 0.1
    kSlowTime = 0x796,   // s16
    kRageKind = 0x89F,   // u8
    kStamina = 0x8A2,    // u16
    kVoiceMute = 0xA69,  // u8
};

constexpr uint32_t kSoundProcs = 0x806A9BB4; // em_snd_proc_tbl: per kind, the handler 7

enum : uint32_t {
    EnemyData = 0x80126098,    // get_enemy_data (work)
    Rand = 0x800CEF20,         // (n)
    StartHandlers = 0x801283A4, // (work, mode, sub): the start handler and the modes 10/11 effects
    ChooseTarget = 0x80128C5C, // (work, VEC3* or 0)
    SetUp = 0x801304DC,        // (work, kind)
    Angry = 0x8012ECA0,        // (work) -> 1
    Waiting = 0x80133C18,      // (work)
    SendAction = 0x803373E0,   // (work, 1, 0): online
    NetPeers = 0x80144560,     // () -> s16
    Defer = 0x801282D8,        // (work, mode, sub): kept for the network's turn
    RoarVoice = 0x801293A0,    // (work, channel, voice, length)
    Adjust8A0 = 0x80130D40,    // (work, s16 delta)
    RotateY = 0x800510C8,      // rotVecY (VEC3*, angle)
    ChangeScale = 0x801359B4,  // get_em_chg_scale (work) -> f1
    MotionSpeed = 0x8012F948,  // (work) -> f1
};

uint8_t R8(uint32_t a) {
    return Memory::Read8(a);
}

uint32_t R32(uint32_t a) {
    return Memory::Read32(a);
}

// 0x80126520: the species' handler `index` from its enemy data's table
// (get_enemy_data(work) +0x10, 23 words; 0 when it has none). Index 7, the
// sound handler, comes from em_snd_proc_tbl by kind instead.
uint32_t Handler(GuestCall& game, uint32_t work, uint32_t index) {
    const uint32_t table = R32(game.Call<EnemyData>({work}) + 0x10);

    if (table == 0 || index > 0x16)
        return 0;

    if (index == 7)
        return R32(kSoundProcs + 4 * R8(work + kKind));

    return R32(table + 4 * index);
}

void HandlerPort(CpuContext* ctx) {
    GuestCall game{ctx};
    const uint32_t handler = Handler(game, ctx->gpr[3], ctx->gpr[4] & 0xFF);
    ctx->gpr[3] = handler;
}

// The sub of the waiting action an action's end and the AI's {0, 0xFF} pick
// go back to on the ground: 2 angry (0x8012ECA0 with +0x1C8 & 0x40), 1 when
// +0x43D is 1, else 0.
uint8_t WaitSub(GuestCall& game, uint32_t work) {
    if (game.Call<Angry>({work}) == 1 && (R32(work + kFlags) & 0x40))
        return 2;

    return R8(work + kWaitSub) == 1 ? 1 : 0;
}

// 0x80128624. `caller` is the original's return address (the monster
// control's trace and `ai off` read it).
void SetAction(CpuContext* ctx, uint32_t work, uint8_t mode, uint8_t sub, uint8_t how, uint32_t caller) {
#ifdef MH3_DEVELOPMENT
    if (MonsterControl::Blocked(work, caller)) [[unlikely]]
        return;

    const MonsterControl::ActionStart start = MonsterControl::Begin(work, mode, sub, how, caller);
#endif

    GuestCall game{ctx};
    GuestFrame frame(ctx, 0x30);
    const uint32_t pair = frame[8]; // the remap handler changes it in place
    Memory::Write8(pair, mode);
    Memory::Write8(pair + 1, sub);

    [&] {
        // Dying or captured (mode 11): only a spawn's start, or 12.0xFF.
        if (how != 1 && R8(work + kMode) == 11 && !(mode == 12 && sub == 0xFF))
            return;

        Memory::Write8(work + kPending, 0);

        if (how == 0 || how == 5) {
            if (R8(work + kHunting) == 0)
                return;

            const uint8_t rage = R8(work + kRageKind);

            if (rage == 1) {
                if (game.Call<0x8012ECC4>({work}) == 0 && game.Call<0x8012CF68>({work, 4}) == 0)
                    game.Call<0x80137784>({work, 2});
            } else if (rage == 3 && Memory::Read16(work + kStamina) != 0)
                game.Call<0x80137784>({work, 0});
        }

        // The species may remap the pair, until it stops changing it.
        if ((R8(work + kHunting) == 1 && how != 3) || how == 1)
            if (const uint32_t remap = Handler(game, work, 4)) {
                uint8_t m, s;

                do {
                    m = R8(pair);
                    s = R8(pair + 1);
                    game(remap, {work, pair, pair + 1});
                } while (m != R8(pair) || s != R8(pair + 1));
            }

        mode = R8(pair);
        sub = R8(pair + 1);

        if (how < 2 || how > 4)
            game.Call<ChooseTarget>({work, 0});

        const uint16_t seed = how == 3 || how == 4 ? Memory::Read16(work + kSeed) : uint16_t(game.Call<Rand>({1}));

        if ((how == 0 || how == 5) && (R32(work + kFlags) & 8)) {
            Memory::Write8(work + kSentMode, mode);
            Memory::Write8(work + kSentMode + 1, sub);
            Memory::Write16(work + kSentMode + 2, seed);
            game.Call<SendAction>({work, 1, 0});

            // Online with others: the action waits for the network's turn.
            if (how != 5 && int16_t(game.Call<NetPeers>()) > 0) {
                game.Call<Defer>({work, mode, sub});
                return;
            }
        }

        for (const uint32_t reset : {0x801297A8u, 0x801322C8u, 0x801321DCu, 0x80132208u, 0x80132234u, 0x80132260u})
            game(reset, {work});

        Memory::Write8(work + kTurnFlags, 0);

        if (how == 2)
            Memory::Write8(work + kStarted, 1);
        else {
            game.Call<0x80137740>({work, 2});

            // Four states the new action does not keep end (each pair: its
            // test of the action, then of the state's flag).
            if (game.Call<0x8012E13C>({mode, sub}) == 0 && game.Call<0x80132330>({work, 4}) != 0)
                game.Call<0x80132CF0>({work});

            if (game.Call<0x8012E1BC>({mode, sub}) == 0 && game.Call<0x80132330>({work, 1}) != 0)
                game.Call<0x80132B34>({work});

            if (game.Call<0x8012E30C>({mode, sub}) == 0 && game.Call<0x80132330>({work, 8}) != 0)
                game.Call<0x80133434>({work});

            if (game.Call<0x8012E338>({mode, sub}) == 0 && game.Call<0x80132330>({work, 0x10}) != 0)
                game.Call<0x80132F5C>({work});

            if (how == 4) {
                if (game.Call<0x8012CF68>({work, 2}) == 1 && int16_t(Memory::Read16(work + kSlowTime)) <= 0)
                    game.Call<0x8012CF00>({work, 2});

                if (game.Call<0x8012CF68>({work, 8}) == 1)
                    game.Call<0x8012CF00>({work, 8});
            }

            Memory::Write8(work + kTarget, 3);
            Memory::Write8(work + kPlace, 0);
            Memory::Write8(work + kStarted, 0);
        }

        // 0x80128604: the action and its random word, its routine at 0.
        Memory::Write8(work + kRoutine, 0);
        Memory::Write8(work + kRoutine + 1, 0);
        Memory::Write8(work + kRoutine + 2, 0);
        Memory::Write8(work + kMode, mode);
        Memory::Write8(work + kSub, sub);
        Memory::Write16(work + kSeed, seed);
        game.Call<StartHandlers>({work, mode, sub});
    }();

#ifdef MH3_DEVELOPMENT
    MonsterControl::NoteAction(ctx, start);
#endif
}

void SetActionPort(CpuContext* ctx) {
    SetAction(ctx, ctx->gpr[3], uint8_t(ctx->gpr[4]), uint8_t(ctx->gpr[5]), uint8_t(ctx->gpr[6]), ctx->lr);
}

// 0x80128B10: back to the waiting action `mode.sub` (how 2), unless dying or
// captured.
void BackTo(CpuContext* ctx, uint32_t work, uint8_t mode, uint8_t sub, uint32_t caller) {
    if (R8(work + kMode) != 11)
        SetAction(ctx, work, mode, sub, 2, caller);
}

void BackToPort(CpuContext* ctx) {
    BackTo(ctx, ctx->gpr[3], uint8_t(ctx->gpr[4]), uint8_t(ctx->gpr[5]), ctx->lr);
}

// 0x80127FAC: an action's end. The monster's motion set back (0x801304DC
// kind 0), then the waiting action 0.WaitSub (the original's three calls
// return to 0x80127FF8, 0x80128018 and 0x8012802C), then 0x80133C18.
void ActionDone(CpuContext* ctx, uint32_t work) {
    GuestCall game{ctx};
    game.Call<SetUp>({work, 0});
    const uint8_t sub = WaitSub(game, work);
    BackTo(ctx, work, 0, sub, sub == 2 ? 0x80127FF8u : sub == 1 ? 0x80128018u : 0x8012802Cu);
    game.Call<Waiting>({work});
}

void ActionDonePort(CpuContext* ctx) {
    ActionDone(ctx, ctx->gpr[3]);
}

// 0x8013C4BC: the AI's pick, the pair at `pair`. {0, 0xFF} picks the
// waiting action for where the monster is: in water (+0x1E2 2) 0.5 angry
// or 0.4, at +0x1E2 1 0.8, 0.9 or 0.3 by +0x1E3, at 3 0.6, at 4 0.7, else
// 0.WaitSub.
void AiPick(CpuContext* ctx, uint32_t work, uint32_t pair) {
    GuestCall game{ctx};
    const uint8_t mode = R8(pair);
    uint8_t sub = R8(pair + 1);

    if (mode == 0 && sub == 0xFF) {
        switch (R8(work + kMedium)) {
        case 2:
            sub = game.Call<Angry>({work}) == 1 && (R32(work + kFlags) & 0x40) ? 5 : 4;
            break;
        case 1:
            sub = R8(work + kMediumSub) == 2 ? 8 : R8(work + kMediumSub) == 1 ? 9 : 3;
            break;
        case 3:
            sub = 6;
            break;
        case 4:
            sub = 7;
            break;
        default:
            sub = WaitSub(game, work);
            break;
        }
    }

    SetAction(ctx, work, mode, sub, 0, MonsterControl::kAiPickReturn);
}

void AiPickPort(CpuContext* ctx) {
    AiPick(ctx, ctx->gpr[3], ctx->gpr[4]);
}

// 0x801D8F00: Deviljho's tail spin (action 7.1). State 0 starts motion 0x15
// with its two voices, its stamina less 20, and its glide (+0x310, +0x31C
// turned to its yaw; forward speed -2.63 × the motion speed × its size).
// State 1 turns it half round over frames 24-100 (0x80133FB0, by game time:
// enemy_turn.cpp), stops the glide from frame 24 on (0x8013548C), starts
// the second voice once and ends with the motion.
void TailSpin(CpuContext* ctx, uint32_t work) {
    GuestCall game{ctx};
    Em::Enemy em{game, work};

    switch (em.State()) {
    case 0: {
        em.State(1);
        Memory::Write8(work + kRoutine + 1, 0);
        game.Call<SetUp>({work, 0});
        em.SetMotion(0x15, 4, 0);
        game.Call<RoarVoice>({work, 0, 2, 0x88});
        game.Call<RoarVoice>({work, 1, 0x34, 0x18});
        game.Call<Adjust8A0>({work, uint32_t(-20)});
        game.Call<0x8013545C>({work});
        game.Call<ChangeScale>({work});
        const float scale = game.F1();
        game.Call<MotionSpeed>({work});
        Memory::WriteFloat32(work + 0x318, -2.6315789f * game.F1() * scale);
        Memory::WriteFloat32(work + 0x324, 0.0f);
        game.Call<RotateY>({work + 0x310, R32(work + kYaw)});
        game.Call<RotateY>({work + 0x31C, R32(work + kYaw)});
        break;
    }
    case 1:
        if (em.At(24.0f, 3, 100.0f))
            game.Call<0x8013548C>({work});

        game.Call<0x80133FB0>({work, 0x8000}, {24.0, 100.0});

        if (R8(work + kRoutine + 1) == 0 && R8(work + kVoiceMute) == 0) {
            Memory::Write8(work + kRoutine + 1, 1);
            game.Call<RoarVoice>({work, 1, 0x35, 0x10});
        }

        if (em.Ended())
            ActionDone(ctx, work);

        break;
    default:
        break;
    }
}

void TailSpinPort(CpuContext* ctx) {
    TailSpin(ctx, ctx->gpr[3]);
}

} // namespace

extern "C" void func_80128624(CpuContext* ctx);
extern "C" void MH3_EnemySetAction(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemySetAction", func_80128624, SetActionPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80128624, MH3_EnemySetAction, "EnemySetAction");

extern "C" void func_80126520(CpuContext* ctx);
extern "C" void MH3_EnemyHandler(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyHandler", func_80126520, HandlerPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80126520, MH3_EnemyHandler, "EnemyHandler");

extern "C" void func_80128B10(CpuContext* ctx);
extern "C" void MH3_EnemyBackTo(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyBackTo", func_80128B10, BackToPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80128B10, MH3_EnemyBackTo, "EnemyBackTo");

extern "C" void func_80127FAC(CpuContext* ctx);
extern "C" void MH3_EnemyActionDone(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyActionDone", func_80127FAC, ActionDonePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80127FAC, MH3_EnemyActionDone, "EnemyActionDone");

extern "C" void func_8013C4BC(CpuContext* ctx);
extern "C" void MH3_EnemyAiPick(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyAiPick", func_8013C4BC, AiPickPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8013C4BC, MH3_EnemyAiPick, "EnemyAiPick");

extern "C" void func_801D8F00(CpuContext* ctx);
extern "C" void MH3_Em007TailSpin(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em007TailSpin", func_801D8F00, TailSpinPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801D8F00, MH3_Em007TailSpin, "Em007TailSpin");
