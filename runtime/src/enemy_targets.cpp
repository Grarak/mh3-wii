// Where a monster heads, RMHP08. C reconstructions from the disassembly:
//
//   0x80128C5C EnemyTargetPoint(em, point): the point its action moves to
//              (+0x36C): `point`, or by the target's kind (+0x380, with
//              +0x381, +0x382): 0 and 4 the first hunter, 1 a hunter it
//              senses, 2 the shared target at 0x806BF958, 3 another monster,
//              5 a point by +0x381, 7 a map object, 8 a route point, 9 a route
//              towards area +0x381, 10 an area's own point, 0xFF ahead. Bit 0
//              of +899's low bits then draws a seed (+0x388), and low 1 or 2
//              moves the point within a radius (+900)
//   0x8013C844 EnemyAreaGoal(em, step): its next area (+0x9F7) and goal area
//              (+0x9F8) by an AI step's kind
//   0x8013DAE0 EnemyAiStepLeave(em, step) -> whether the AI step's table
//              gave a follow-up
#include "abi_bridge.h"
#include "guest_call.h"
#include "guest_fields.h"
#include "port_check.h"

#include <cstdint>

namespace {

using namespace GuestFields;

enum : uint32_t {
    kArea = 0x1E1,
    kPosition = 0x188,
    kYaw = 0x1C0,
    kGoal = 0x36C,   // VEC3
    kKind = 0x380,   // u8
    kWhich = 0x381,  // u8
    kIndex = 0x382,  // u8
    kMove = 0x383,   // u8: low bits, 0x80 the move's own flag
    kRadius = 0x384, // f32
    kSeed = 0x388,   // u16
    kNext = 0x9F7,   // u8
    kFinal = 0x9F8,  // u8
};

enum : uint32_t {
    Vec3Copy = 0x80041EA4,
    Vec3Set = 0x8005000C,
    Vec3Add = 0x800513DC,
    Vec3Sub = 0x80050D04,
    Vec3Scale = 0x80051F44,
    RotateY = 0x800510C8,
    MoveWork = 0x800CFAF4,
    Rand = 0x800CEF20,
    NextArea = 0x80126E10,
    NearArea = 0x80127328,
    RoutePoint = 0x801262DC,
    AreaPoint = 0x80127C80,
    IconOf = 0x80343A38,
    SlotOf = 0x80343948,
};

constexpr uint32_t kShared = 0x806BF7E0, kSharedArea = 0x806BF984, kSharedPoint = 0x806BF958;

// `offset` turned by `angle` from the monster: the goal.
void GoalAround(GuestCall& game, GuestFrame& frame, uint32_t em, float z, uint32_t angle) {
    const uint32_t offset = frame[168], sum = frame[108];
    WF(offset, 0.0f);
    WF(offset + 4, 0.0f);
    WF(offset + 8, z);
    game.Call<RotateY>({offset, angle});
    game.Call<Vec3Add>({sum, em + kPosition, offset});
    Copy3(em + kGoal, sum);
}

void ZeroGoal(uint32_t em) {
    for (uint32_t k = 0; k < 12; k += 4)
        WF(em + kGoal + k, 0.0f);
}

uint32_t RouteKey(uint32_t area, uint32_t index) {
    return ((area & 0xF) << 8 | index) & 0xFFFF;
}

// A map object's point, or between the two ends it holds. True for the ends
// (at `a` and `b`).
bool ObjectPoint(GuestCall& game, GuestFrame& frame, uint32_t em, uint32_t a, uint32_t b, bool& none) {
    const uint32_t slot = game.Call<SlotOf>({game.Call<IconOf>({R8(em + 3), R8(em + 10)}) & 0xFF, R8(em + 0x46C)});

    if (!slot || R8(em + kArea) != R8(slot + 0x14)) {
        none = true;
        return false;
    }

    if (R8(slot + 0x36) != 0xFF) {
        const uint32_t ends = game.Call<0x80345718>({slot, R8(slot + 0x36)});

        if (ends && R8(ends) == 1) {
            game.Call<Vec3Set>({a}, {RF(ends + 4), RF(ends + 8), RF(ends + 0xC)});
            game.Call<Vec3Set>({b}, {RF(ends + 0x10), RF(ends + 0x14), RF(ends + 0x18)});
            W32(em + kRadius, R32(ends + 0x1C));
            game.Call<Vec3Add>({frame[84], a, b});
            game.Call<Vec3Scale>({frame[96], frame[84]}, {0.5});
            Copy3(em + kGoal, frame[96]);
            return true;
        }
    }

    W32(em + kRadius, R32(slot + 0x30));
    Copy3(em + kGoal, slot + 0x24);
    return false;
}

// Kind 5. False when the goal stays as it is and no move follows.
bool PointByWhich(GuestCall& game, GuestFrame& frame, uint32_t em, uint32_t a, uint32_t b, bool& ends, bool& keepHere) {
    const uint32_t which = R8(em + kWhich), index = R8(em + kIndex);

    switch (which) {
    case 0:
    case 1:
    case 2:
    case 3: {
        static constexpr uint32_t kTurns[]{0x4000, 0xFFFFC000, 0, 0x8000};
        GoalAround(game, frame, em, 100.0f * float(index), R32(em + kYaw) + kTurns[which]);
        return true;
    }
    case 6:
        Copy3(em + kGoal, em + 0x1B0);
        return true;
    case 7: {
        bool none = false;
        ends = ObjectPoint(game, frame, em, a, b, none);
        return !none;
    }
    case 8:
        game.Call<RoutePoint>({em, RouteKey(R8(em + kArea), index), em + kGoal});
        return true;
    case 9:
        if (game.Call<AreaPoint>({em, R8(em + kArea), em + kGoal}) == 0)
            ZeroGoal(em);

        return true;
    case 10:
    case 11:
        GoalAround(game, frame, em, 500.0f, index * 0x100 + (which == 10 ? R32(em + kYaw) : 0));
        return true;
    case 12:
    case 13:
        W8(em + kMove, which == 13 ? 0x81 : 1);
        WF(em + kRadius, 100.0f * float(index));
        keepHere = true;
        Copy3(em + kGoal, em + kPosition);
        return true;
    default:
        return true;
    }
}

// Kind 9: a route towards the area +0x381, seen from this area.
void PointToArea(GuestCall& game, GuestFrame& frame, uint32_t em) {
    const uint32_t area = R8(em + kArea), which = R8(em + kWhich), index = R8(em + kIndex);

    if (index == 0xFF || which != R8(em + kFinal)) {
        if (game.Call<AreaPoint>({em, which, em + kGoal}) == 0)
            ZeroGoal(em);
    } else
        game.Call<RoutePoint>({em, RouteKey(which, index), em + kGoal});

    if (area == which) {
        game.Call<0x802B0494>({area});
        WF(em + 0x370, game.F1());
        W8(em + 0x1FA, 0);
        return;
    }

    const uint32_t here = frame[60], there = frame[48], way = frame[24], sum = frame[120];
    game.Call<0x802B0164>({here, em + kPosition, area});
    game.Call<0x802B0164>({there, em + kGoal, which});
    game.Call<Vec3Sub>({way, there, here});
    game.Call<Vec3Add>({sum, em + kPosition, way});
    Copy3(em + kGoal, sum);
    game.Call<0x802B0494>({area});
    WF(em + 0x370, 2000.0f + game.F1());
    W8(em + 0x1FA, 1);
}

bool HunterGoal(GuestCall& game, uint32_t em) {
    const uint32_t hunter = game.Call<MoveWork>({2});

    if (R8(hunter) == 0)
        return false;

    Copy3(em + kGoal, hunter + 0x3C);
    return true;
}

// The goal by kind. False when there is none, nothing following.
bool GoalByKind(GuestCall& game, GuestFrame& frame, uint32_t em, uint32_t a, uint32_t b, bool& ends, bool& keepHere) {
    const uint32_t kind = R8(em + kKind), which = R8(em + kWhich), index = R8(em + kIndex);

    switch (kind) {
    case 1: {
        if (index == 0xFF)
            return false;

        const uint32_t hunter = game.Call<0x80137834>({which});

        if (R8(hunter) == 0 || game.Call<0x8012D118>({em, hunter}) != 1)
            return false;

        Copy3(em + kGoal, hunter + 0x3C);
        return true;
    }
    case 2:
        if (index == 0xFF || R8(kShared) == 0 || R8(em + kArea) != R8(kSharedArea))
            return false;

        Copy3(em + kGoal, kSharedPoint);
        return true;
    case 3: {
        if (index == 0xFF)
            return false;

        const uint32_t other = game.Call<MoveWork>({3}) + index * 0xB18;

        if (R8(other) == 0 || R8(em + kArea) != R8(other + kArea))
            return false;

        Copy3(em + kGoal, other + kPosition);
        return true;
    }
    case 5:
        return PointByWhich(game, frame, em, a, b, ends, keepHere);
    case 6:
        return false;
    case 7: {
        const uint32_t object = game.Call<0x802AB9F8>({which, index});

        if (!object)
            return false;

        Copy3(em + kGoal, R32(object) + 0x18);
        return true;
    }
    case 8:
        game.Call<0x801271BC>({em, which});

        if (R32(em + 0x9A4) == 0)
            return false;

        game.Call<RoutePoint>({em, RouteKey(R8(em + kArea), R16(R32(R32(em + 0x9A4) + 4) + index * 0xC) & 0xFF), em + kGoal});
        return true;
    case 9:
        PointToArea(game, frame, em);
        return true;
    case 10:
        game.Call<0x801275F0>({em, R8(em + kArea), frame[8]});
        game.Call<0x80127594>({em, R32(frame[8]), index, em + kGoal});
        return true;
    case 0xFF:
        GoalAround(game, frame, em, 1000.0f, R32(em + kYaw));
        return true;
    default:
        return HunterGoal(game, em);
    }
}

void TargetPoint(CpuContext* ctx) {
    GuestFrame frame(ctx, 0xE0);
    GuestCall game{ctx};
    const uint32_t em = frame.Arg(3), point = frame.Arg(4), a = frame[156], b = frame[144];
    bool ends = false, keepHere = false;

    if (point)
        game.Call<Vec3Copy>({em + kGoal, point});
    else if (!GoalByKind(game, frame, em, a, b, ends, keepHere))
        return;

    const uint32_t state = R8(em + kMove) & 0x7F;

    if (state == 1) {
        W8(em + kMove, (R8(em + kMove) & 0x80) | 2);
        W16(em + kSeed, game.Call<Rand>({1}));
    } else if (state != 2)
        return;

    const uint32_t own = R8(em + kMove) >> 7, seed = R16(em + kSeed);
    const double radius = double(RF(em + kRadius));

    if (ends)
        game.Call<0x80135880>({em + kGoal, a, b, own, seed}, {radius});
    else if (keepHere)
        game.Call<0x80135844>({em + kGoal, em + kGoal, own, seed}, {radius});
    else
        game.Call<0x801357C8>({em + kGoal, em + kGoal, own, seed}, {radius});
}

void AreaGoal(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x30);
    GuestCall game{ctx};
    const uint32_t em = frame.Arg(3), step = frame.Arg(4);
    const uint32_t area = R8(em + kArea);

    switch (R8(step)) {
    case 1: {
        const uint32_t goal = R8(step + 1) == 0xFD ? R8(em + 0x1FF) : R8(step + 1);
        W8(em + kFinal, goal);
        W8(em + kNext, game.Call<NextArea>({em, area, goal}));
        break;
    }
    case 2: {
        const uint32_t goal = R8(step + 1) == 0xFD ? R8(em + 0x1FF) : R8(step + 1);
        uint32_t next = game.Call<NextArea>({em, area, goal}) & 0xFF;
        W8(em + kNext, next);
        W8(em + kFinal, goal);

        for (; next != goal; next = game.Call<NextArea>({em, next, goal}) & 0xFF)
            if (game.Call<0x80126FE4>({em, R8(em + 0x1E0), next}) == 1) {
                W8(em + kFinal, next);
                return;
            }

        break;
    }
    case 3: {
        const uint32_t near = game.Call<NearArea>({em, em + kPosition, area});
        W8(em + kNext, near);
        W8(em + kFinal, near);
        break;
    }
    case 4: {
        const uint32_t slot = game.Call<SlotOf>({game.Call<IconOf>({R8(em + 3), R8(em + 10)}) & 0xFF, R8(em + 0x46C)});

        if (slot) {
            const uint32_t goal = game.Call<0x80345400>({slot});
            W8(em + kFinal, goal);
            W8(em + kNext, game.Call<NextArea>({em, area, goal & 0xFF}));
        } else {
            const uint32_t near = game.Call<NearArea>({em, em + kPosition, area});
            W8(em + kNext, near);
            W8(em + kFinal, near);
        }

        break;
    }
    case 5:
        W8(em + kNext, 0xFE);
        W8(em + kFinal, 0xFE);
        break;
    default:
        W8(em + kFinal, 0xFF);
        W8(em + kNext, 0xFF);
        break;
    }
}

uint32_t StepLeave(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x30);
    GuestCall game{ctx};
    const uint32_t em = frame.Arg(3), step = frame.Arg(4), cursor = frame[8], result = frame[12];
    const uint32_t kind = R8(step);
    W16(result, 0);
    W32(cursor, step);

    if (kind == 0) {
        const uint32_t slot = game.Call<SlotOf>({game.Call<IconOf>({R8(em + 3), R8(em + 10)}) & 0xFF, R8(em + 0x46C)});

        if (!slot || R8(slot + 0x14) != R8(em + kArea)) {
            W32(cursor, step + (game.Call<0x80140744>({0x25, kind}) & 0xFF));
            game.Call<0x8013BE48>({cursor, 0x25, result});
        }
    } else if (kind == 2) {
        W32(cursor, step + (game.Call<0x80140744>({0x25, kind}) & 0xFF));
        W16(result, game.Call<0x801407DC>({R32(cursor), 0x25, 1}));
    } else if (kind != 0xFF)
        game.Call<0x80140B5C>({em, 0, 0});

    return uint32_t(S16(result));
}

} // namespace

extern "C" void func_80128C5C(CpuContext* ctx);
extern "C" void MH3_EnemyTargetPoint(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyTargetPoint", func_80128C5C, TargetPoint);
}
REGISTER_NATIVE_FUNCTION_AS(0x80128C5C, MH3_EnemyTargetPoint, "EnemyTargetPoint");

extern "C" void func_8013C844(CpuContext* ctx);
extern "C" void MH3_EnemyAreaGoal(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyAreaGoal", func_8013C844, AreaGoal);
}
REGISTER_NATIVE_FUNCTION_AS(0x8013C844, MH3_EnemyAreaGoal, "EnemyAreaGoal");

extern "C" void func_8013DAE0(CpuContext* ctx);
extern "C" void MH3_EnemyAiStepLeave(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyAiStepLeave", func_8013DAE0, [](CpuContext* c) {
        const uint32_t result = StepLeave(c);
        c->gpr[3] = result;
    });
}
REGISTER_NATIVE_FUNCTION_AS(0x8013DAE0, MH3_EnemyAiStepLeave, "EnemyAiStepLeave");
