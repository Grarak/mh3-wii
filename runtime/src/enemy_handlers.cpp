// Species handlers and small monster moves, RMHP08. C reconstructions from
// the disassembly:
//
//   0x8017643C JaggiPickArea(em, n): handler 0x15 of Great Jaggi's family:
//              1 when it should move (+0x1E4 says whether to go on, +0x1FC,
//              +0x1FE and +0x1FF the move), on the map kinds it leaves its
//              area in (1, 2 and 4)
//   0x80175ED8 JaggiCondition(em, id): handler 9: 0 +0x1E4 is 1, 1 fewer than
//              two Jaggi (kind 10) are out with it, 2 by 0x80130198 outside
//              its home areas
//   0x801760F4 JaggiFirstAction(em, u8* mode, u8* sub): handler 0xC: its
//              starting action and place by the map
//   0x80170674 JaggiStart(em, how): handler 1
//   0x801707BC JaggiActionStart(em, mode, sub): handler 5
//   0x80192920 EnemyActionStart(em, mode, sub): a species' handler 5
//   0x803373E0 EnemyNetSend(em, kind, arg): online, one of seven messages
//   0x8016CBEC EnemyLeaveArea(em): 1 with the area to leave for in +0x1FE
//              and +0x1FF, on the map kinds with routes
//   0x8016CDB8 EntryMove(entry): a held small monster's table entry follows
//              its group to another area after a while
#include "abi_bridge.h"
#include "game_step.h"
#include "guest_call.h"
#include "guest_fields.h"
#include "port_check.h"

#include <cstdint>

namespace {

using namespace GuestFields;

enum : uint32_t {
    kArea = 0x1E1,
    kGoesOn = 0x1E4,  // u8
    kMoves = 0x1FC,   // u8
    kMoveKind = 0x1FE,
    kMoveArea = 0x1FF,
    kSlot = 0x46C,    // u8
    kWorkBytes = 0xB18,
};

enum : uint32_t {
    MapKind = 0x802B06CC,
    IconOf = 0x80343A38,
    SlotOf = 0x80343948,
    MoveWork = 0x800CFAF4,
    MoveCount = 0x800CFB34,
    Rand = 0x800CEF20,
    Shortcut = 0x8012ED54,
    Vec3Set = 0x8005000C,
};

constexpr uint32_t kEntries = 0x806A7960; // small monsters, in pairs of 0x44 bytes

uint32_t Kind(GuestCall& game, uint32_t em) {
    return game.Call<MapKind>({R8(em + 0x1E0)}) & 0xFF;
}

bool RoutedMap(uint32_t kind) {
    return kind - 1 < 2 || kind == 4;
}

uint32_t Slot(GuestCall& game, uint32_t kind, uint32_t variant, uint32_t index) {
    return game.Call<SlotOf>({game.Call<IconOf>({kind, variant}) & 0xFF, index});
}

void Move(uint32_t em, uint32_t kind, uint32_t area) {
    W8(em + kMoves, 1);
    W8(em + kMoveKind, kind);
    W8(em + kMoveArea, area);
}

// The Jaggi family's way out of its home area on each map kind.
struct Home {
    uint32_t area, first, second;
};

Home HomeOf(uint32_t kind) {
    return kind == 1 ? Home{9, 4, 5} : kind == 2 ? Home{5, 4, 4} : Home{5, 3, 6};
}

// To the map's home area, or on from it by the low bit of `n` once +0x44C is
// out. True when it moves.
bool HeadHome(uint32_t em, uint32_t kind, uint32_t n) {
    if (kind != 1 && kind != 2 && kind != 4)
        return false;

    const Home home = HomeOf(kind);

    if (R8(em + kArea) != home.area)
        Move(em, 1, home.area);
    else if (S16(em + 0x44C) < 1)
        Move(em, 1, n & 1 ? home.second : home.first);
    else
        return false;

    W8(em + kGoesOn, 1);
    return true;
}

bool PackNear(GuestCall& game, uint32_t em) {
    uint32_t other = game.Call<MoveWork>({3});
    const uint32_t count = game.Call<MoveCount>({3}) & 0xFFFF, kind = R8(em + 3);

    for (uint32_t i = 0; i < count; ++i, other += kWorkBytes) {
        const uint32_t mode = R8(other + 0x1E5);

        if (R8(other) == 0 || mode == 11 || mode == 12 || R8(other + kArea) != R8(em + kArea))
            continue;

        if (kind == 12 ? R8(other + 3) - 10 < 2 : kind == 14 && R8(other + 3) == 13)
            return true;
    }

    return false;
}

// The slot's call to rejoin. 1 or 0 as the pick's result, -1 to go on.
int32_t Rejoin(GuestCall& game, uint32_t em, uint32_t kind, uint32_t n) {
    if (S16(em + 0x450) > S16(em + 0x452) || R8(em + 0x43D) != 1)
        return -1;

    const uint32_t slot = Slot(game, R8(em + 3), R8(em + 10), R8(em + kSlot));

    if (!slot || R8(slot + 0xD) != 1 || PackNear(game, em) || game.Call<0x80129A80>({em, n & 0xFFFF, 0x1E, em + 0x328}) != 1)
        return -1;

    if (game.Call<Shortcut>({em}) != 1) {
        Move(em, 0, 0xFF);
        W8(em + kGoesOn, 0);
        return 1;
    }

    return HeadHome(em, kind, n) ? 1 : -1;
}

uint32_t PickArea(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x40);
    GuestCall game{ctx};
    const uint32_t em = frame.Arg(3), n = frame.Arg(4), kind = Kind(game, em);

    if (!RoutedMap(kind))
        return 0;

    if (game.Call<0x80129DA0>({em}) == 1) {
        W8(em + kGoesOn, 0);
        return 1;
    }

    const uint32_t home = kind == 1 ? 9 : 5;
    const uint32_t state = game.Call<0x80129E1C>({em, home}) & 0xFF;

    if (state == 2) {
        W8(em + kGoesOn, 1);
        return 1;
    }

    const bool atHome = state == 1;

    if (!atHome) {
        const uint32_t other = game.Call<0x80129EAC>({em}) & 0xFF;

        if (other == 1)
            return 0;

        if (other == 2) {
            W8(em + kGoesOn, 0);
            return 1;
        }
    }

    if (game.Call<Shortcut>({em}) == 0 && S16(em + 0x452) > 899 && game.Call<0x8012A078>({em, 0, home, n & 0xFFFF, 0, frame.Arg(13) - 0x73C8}) == 1) {
        W8(em + kGoesOn, 0);
        return 1;
    }

    if (const int32_t result = Rejoin(game, em, kind, n); result >= 0)
        return uint32_t(result);

    if (game.Call<0x80129AD4>({em, n & 0xFFFF}) == 1) {
        if (game.Call<Shortcut>({em}) == 1 && HeadHome(em, kind, n))
            return 1;

        W8(em + kGoesOn, 0);
        return 1;
    }

    if (game.Call<0x8012A268>({em}) != 1)
        return 0;

    if (atHome) {
        const Home where = HomeOf(kind);
        Move(em, 1, n & 1 ? where.second : where.first);
    }

    W8(em + kGoesOn, atHome);
    return 1;
}

uint32_t JaggiCount(GuestCall& game, uint32_t em) {
    uint32_t count = 0, other = game.Call<MoveWork>({3});
    const uint32_t works = game.Call<MoveCount>({3}) & 0xFFFF, slot = R8(em + kSlot);

    if (other)
        for (uint32_t i = 0; i < works; ++i, other += kWorkBytes)
            if (R8(other) != 0 && R8(other + 3) == 10 && R8(other + kSlot) == slot && R8(other + 0x11) != 0)
                count = (count + 1) & 0xFF;

    for (uint32_t i = 0; i < 0x80; ++i) {
        const uint32_t e = kEntries + i * 0x44;

        if (R8(e) != 0 && R8(e + 2) == 10 && R8(e + 9) == slot && R8(e + 8) == 4)
            count = (count + 1) & 0xFF;
    }

    return count;
}

uint32_t Condition(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const uint32_t em = frame.Arg(3);

    switch (frame.Arg(4) & 0xFF) {
    case 0:
        return R8(em + kGoesOn) == 1;
    case 1: {
        const uint32_t count = JaggiCount(game, em);
        return ((count | 0xFFFFFFFDu) - ((count - 2) >> 1)) >> 31;
    }
    case 2:
        if ((Kind(game, em) == 1 && R8(em + kArea) == 9) || (Kind(game, em) == 2 && R8(em + kArea) == 1))
            return 0;

        return game.Call<0x80130198>({em, 0}) == 1;
    default:
        return 0;
    }
}

void FirstAction(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x30);
    GuestCall game{ctx};
    const uint32_t em = frame.Arg(3), mode = frame.Arg(4), sub = frame.Arg(5);
    game.Call<0x801304DC>({em, 0});
    W8(mode, 0xC);
    const uint32_t kind = Kind(game, em), area = R8(em + kArea);

    struct Start {
        uint32_t kind, area, a, b, sub;
    };

    static constexpr Start kStarts[]{
        {1, 2, 0x12, 0xC, 0}, {1, 3, 0x11, 0xB, 1}, {1, 9, 0x10, 10, 1}, {2, 4, 10, 0xB, 1}, {2, 5, 9, 0xB, 1}, {4, 2, 9, 0xB, 1}, {4, 5, 10, 0x10, 2},
    };

    for (const Start& s : kStarts)
        if (s.kind == kind && s.area == area) {
            game.Call<0x80126388>({em, s.a, s.b}, {0.0});
            W8(sub, s.sub);
            return;
        }

    for (uint32_t k = 0; k < 12; k += 4) {
        WF(em + 0x188 + k, 0.0f);
        W32(em + 0x1BC + k, 0);
    }

    game.Call<0x8012B3E4>({em, 5, 2, 10});
    W8(sub, 0);
}

void Start(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x30);
    GuestCall game{ctx};
    const uint32_t em = frame.Arg(3), mode = frame[9], sub = frame[8], at = frame[12];

    if ((frame.Arg(4) & 0xFF) == 2) {
        game.Call<0x801760F4>({em, mode, sub});
        game.Call<0x80128AF0>({em, R8(mode), R8(sub)});
        game.Call<0x80133C24>({em});
    }

    if (R8(em + 9) == 0) {
        game.Call<Vec3Set>({at}, {0.0, 0.0, 45.0});
        game.Call<0x80105808>({em, 0x14, at, 0x1C72}, {double(0.7f)});
    }
}

void ActionStart(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const uint32_t em = frame.Arg(3);

    if ((frame.Arg(4) & 0xFF) != 1)
        return;

    switch (frame.Arg(5) & 0xFF) {
    case 5:
        game.Call<0x80130FD8>({em});
        break;
    case 10:
        W8(em + 0x32A, 0);
        break;
    case 12:
    case 13:
        if (R8(em + 0x1F5) == 1)
            game.Call<0x8017071C>({em});

        break;
    case 14:
        game.Call<0x80137718>({em});
        break;
    default:
        break;
    }
}

// The first hunter its senses and 0x801924AC accept becomes its target.
void TargetHunter(GuestCall& game, uint32_t em) {
    const uint32_t count = game.Call<MoveCount>({2}) & 0xFFFF;
    uint32_t hunter = game.Call<MoveWork>({2});

    for (uint32_t i = 0; i < count; ++i, hunter += 0xB20) {
        if (R8(hunter) == 0 || game.Call<0x8012D20C>({R8(hunter + 8)}) != 0 || game.Call<0x8012D118>({em, hunter}) == 0 || game.Call<0x8027D594>({hunter}) == 0 ||
            game.Call<0x801924AC>({em}) == 0)
            continue;

        if (S8(em + 0x1F6) != int32_t(i))
            game.Call<0x80125324>({em, i & 0xFF});

        return;
    }
}

void EnemyActionStart(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const uint32_t em = frame.Arg(3), mode = frame.Arg(4) & 0xFF, sub = frame.Arg(5) & 0xFF;

    if (R32(em + 0x334) != 0xFFFFFFFFu) {
        game.Call<0x803B99F8>({R32(em + 0x334)});
        W32(em + 0x334, 0xFFFFFFFFu);
    }

    if (mode == 1) {
        if (sub == 5)
            game.Call<0x80130FD8>({em});
        else if (sub == 7)
            game.Call<0x80137718>({em});
    } else if (mode == 5) {
        switch (sub) {
        case 0xE:
            W16(em + 0x362, 900);
            break;
        case 0x1F:
            if (R8(em + 0x1F5) == 1)
                game.Call<0x802BC814>({em});

            break;
        case 0x20:
            game.Call<0x803A9328>({0x12, 2});
            W16(em + 0x360, 0xA8C);
            break;
        case 0x21:
            game.Call<0x8011E748>({em, 0xFFFFFF9Cu});

            if (int32_t(R32(em + 0x7A0)) < 1)
                game.Call<0x8011E7C4>({em, 1});

            W16(em + 0x362, 900);
            break;
        case 0x24:
            if (R8(em + 0x1F5) == 1)
                TargetHunter(game, em);

            break;
        case 0x25:
            game.Call<0x80137718>({em});
            break;
        default:
            break;
        }
    } else if (mode == 10) {
        if (sub == 0xC8)
            game.Call<0x80135CC0>({em, 0, game.Call<0x80130168>({em}) == 1 ? 3u : 0u});
        else if (sub == 0xD0)
            game.Call<0x80135CC0>({em, 0, 0});
        else if (sub == 0xB2)
            W32(em + 0x20, 0x3C);
    }
}

void NetSend(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const uint32_t em = frame.Arg(3), kind = frame.Arg(4) & 0xFF, arg = frame.Arg(5) & 0xFF;

    if (game.Call<0x8045169C>() == 0 || S16(em + 0x1A) == -1 || game.Call<0x80334AB0>() == 0)
        return;

    static constexpr uint32_t kSenders[]{0x80336100, 0x8033694C, 0x80336D78, 0x80336F04, 0x80336FE8, 0x80337168, 0x80337288};

    if (kind - 1 < 7)
        game(kSenders[kind - 1], {em, game.Call<0x800CF3E8>() & 0xFF, 5, kind, arg});
}

void LeaveTo(GuestCall& game, uint32_t em, uint32_t route, uint32_t area) {
    Move(em, game.Call<0x80176B0C>({route, R8(em + kArea)}) == 1, area);
}

uint32_t LeaveArea(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const uint32_t em = frame.Arg(3);

    if (!RoutedMap(Kind(game, em)))
        return 0;

    if (R8(em + 0x11) != 0) {
        Move(em, 7, 0);
        return 1;
    }

    const uint32_t slot = Slot(game, R8(em + 3), R8(em + 10), R8(em + kSlot));

    if (!slot || R32(slot + 0x18) == 0 || R8(em + kSlot) != R8(slot + 0x17))
        return 0;

    const uint32_t target = R8(slot + (R8(slot + 8) == 5 ? 0x10 : 0x14));

    if (R8(em + kArea) == target)
        return 0;

    LeaveTo(game, em, R32(slot + 0x18), target);
    return 1;
}

void EntryMove(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const uint32_t entry = frame.Arg(3);
    const uint32_t kind = game.Call<MapKind>({game.Call<0x802AFCD8>() & 0xFF}) & 0xFF;

    if (!RoutedMap(kind))
        return;

    const uint32_t slot = Slot(game, R8(entry + 2), R8(entry + 3), R8(entry + 9));

    if (!slot || R8(entry + 0xC) != 3)
        return;

    if (R8(entry + 0x20) == 0) {
        if (R8(entry + 10) != R8(slot + 0x14)) {
            W8(entry + 0x20, 1);
            W16(entry + 0x22, (game.Call<Rand>({1}) & 0x3F) + 0xF);
        }

        return;
    }

    if (R8(entry + 0x20) != 1)
        return;

    const int32_t left = int16_t(S16(entry + 0x22) - int32_t(GameStep::Ticks()));
    W16(entry + 0x22, uint32_t(left));

    if (left >= 0)
        return;

    W8(entry + 0x20, 0);
    W16(entry + 0x22, 0);
    const uint32_t target = R8(slot + (R8(slot + 0xF) == 3 ? 0x10 : 0x14));
    const uint32_t seed = game.Call<Rand>({1}) & 0xFFFF;
    const uint32_t area = game.Call<0x80126EA0>({R8(entry + 2), R8(entry + 3), R8(entry + 10), target, seed}) & 0xFF;

    if (game.Call<0x80126C38>({entry, R8(slot + 0x13), area}) == 1) {
        W8(entry + 0xB, R8(entry + 10));
        W8(entry + 10, area);
    }
}

template <uint32_t (*F)(CpuContext*)>
void Returning(CpuContext* ctx) {
    const uint32_t result = F(ctx);
    ctx->gpr[3] = result;
}

} // namespace

extern "C" void func_8017643C(CpuContext* ctx);
extern "C" void MH3_JaggiPickArea(CpuContext* ctx) {
    PortCheck::Call(ctx, "JaggiPickArea", func_8017643C, Returning<PickArea>);
}
REGISTER_NATIVE_FUNCTION_AS(0x8017643C, MH3_JaggiPickArea, "JaggiPickArea");

extern "C" void func_80175ED8(CpuContext* ctx);
extern "C" void MH3_JaggiCondition(CpuContext* ctx) {
    PortCheck::Call(ctx, "JaggiCondition", func_80175ED8, Returning<Condition>);
}
REGISTER_NATIVE_FUNCTION_AS(0x80175ED8, MH3_JaggiCondition, "JaggiCondition");

extern "C" void func_801760F4(CpuContext* ctx);
extern "C" void MH3_JaggiFirstAction(CpuContext* ctx) {
    PortCheck::Call(ctx, "JaggiFirstAction", func_801760F4, FirstAction);
}
REGISTER_NATIVE_FUNCTION_AS(0x801760F4, MH3_JaggiFirstAction, "JaggiFirstAction");

extern "C" void func_80170674(CpuContext* ctx);
extern "C" void MH3_JaggiStart(CpuContext* ctx) {
    PortCheck::Call(ctx, "JaggiStart", func_80170674, Start);
}
REGISTER_NATIVE_FUNCTION_AS(0x80170674, MH3_JaggiStart, "JaggiStart");

extern "C" void func_801707BC(CpuContext* ctx);
extern "C" void MH3_JaggiActionStart(CpuContext* ctx) {
    PortCheck::Call(ctx, "JaggiActionStart", func_801707BC, ActionStart);
}
REGISTER_NATIVE_FUNCTION_AS(0x801707BC, MH3_JaggiActionStart, "JaggiActionStart");

extern "C" void func_80192920(CpuContext* ctx);
extern "C" void MH3_EnemyActionStart(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyActionStart", func_80192920, EnemyActionStart);
}
REGISTER_NATIVE_FUNCTION_AS(0x80192920, MH3_EnemyActionStart, "EnemyActionStart");

extern "C" void func_803373E0(CpuContext* ctx);
extern "C" void MH3_EnemyNetSend(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyNetSend", func_803373E0, NetSend);
}
REGISTER_NATIVE_FUNCTION_AS(0x803373E0, MH3_EnemyNetSend, "EnemyNetSend");

extern "C" void func_8016CBEC(CpuContext* ctx);
extern "C" void MH3_EnemyLeaveArea(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyLeaveArea", func_8016CBEC, Returning<LeaveArea>);
}
REGISTER_NATIVE_FUNCTION_AS(0x8016CBEC, MH3_EnemyLeaveArea, "EnemyLeaveArea");

extern "C" void func_8016CDB8(CpuContext* ctx);
extern "C" void MH3_EntryMove(CpuContext* ctx) {
    PortCheck::Call(ctx, "EntryMove", func_8016CDB8, EntryMove);
}
REGISTER_NATIVE_FUNCTION_AS(0x8016CDB8, MH3_EntryMove, "EntryMove");
