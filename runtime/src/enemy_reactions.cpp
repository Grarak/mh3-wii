// A monster's reaction to damage, RMHP08. C reconstruction from the
// disassembly: 0x80123150 EnemyReactionStart(em) turns the reaction
// EnemyDamageResolve asked for (u16 +0x82C, enemy_parts.cpp) into an action
// (0x80128B30(em, mode, sub), mode 10 the reactions, 11 the deaths) by where
// the monster is (u8 +0x1E2: 0 land, 1 a ledge by +0x1E3, 2 water, 3 and 4 the
// others). 1 when it started one. Reactions 1, 2-5, 0x11-0x17 and 0x1B have
// their own functions.
#include "abi_bridge.h"
#include "guest_call.h"
#include "guest_fields.h"
#include "port_check.h"

#include <cstdint>

namespace {

using namespace GuestFields;

enum : uint32_t {
    kPlace = 0x1E2,  // u8
    kLedge = 0x1E3,  // u8
    kReaction = 0x82C,
    kAt81E = 0x81E,  // s16
    kStagger = 0x832,
    SetAction = 0x80128B30,
    State934 = 0x8012D934,
    StateDBC = 0x8012DDBC,
    KindE0A4 = 0x8012E0A4,
    State240 = 0x80132240,
    HasReactions = 0x8011E6A4,
    Pick = 0x80120594,
    Mark7DC = 0x801307DC,
};

// The actions of a reaction by place.
struct Placed {
    uint32_t state240, ledgeHigh, ledgeLow, ledge, water, place4, land;
};

constexpr Placed kReaction10{0xAE, 0xAC, 0xAD, 0xA9, 0xAA, 0xAB, 0xA8};
constexpr Placed kReaction11{0xB5, 0xB3, 0xB4, 0xB0, 0xB1, 0xB2, 0xAF};
constexpr Placed kReaction19{0xE3, 0xE1, 0xE2, 0xDE, 0xDF, 0xE0, 0xDD};

struct Start {
    GuestCall& game;
    uint32_t em;

    uint32_t Action(uint32_t mode, uint32_t sub) const {
        game.Call<SetAction>({em, mode, sub});
        return 1;
    }

    uint32_t Bits(uint32_t bits) const { return game.Call<HasReactions>({em, bits}); }
    uint32_t Call(uint32_t f) const { return game(f, {em}); }

    uint32_t ByPlace(const Placed& p) const {
        if (game.Call<State240>({em}) == 1)
            return Action(10, p.state240);

        const uint32_t place = R8(em + kPlace);

        if (place == 1) {
            if (R8(em + kLedge) == 2)
                return Action(10, p.ledgeHigh);

            if (R8(em + kLedge) == 1)
                return Action(10, p.ledgeLow);
        } else if (place != 3) {
            return Action(10, place == 2 ? p.water : place == 4 ? p.place4 : p.land);
        }

        return Action(10, p.ledge);
    }

    // By 0x80120594(em, kind): 0, 1, or the others' action.
    uint32_t ByPick(uint32_t mode, uint32_t kind, uint32_t first, uint32_t second, uint32_t other) const {
        const uint32_t s = game.Call<Pick>({em, kind}) & 0xFF;
        return Action(mode, s == 0 ? first : s == 1 ? second : other);
    }

    uint32_t ByKind(uint32_t first, uint32_t second) const {
        const uint32_t pick = game.Call<KindE0A4>({em}) & 0xFF;

        if (pick > 1)
            return 0;

        return Action(10, pick == 0 ? first : second);
    }

    uint32_t Reaction7() const {
        if (game.Call<State934>({em}) == 1)
            return Action(10, 0xBC);

        if (game.Call<StateDBC>({em}) == 1)
            return ByKind(0xE9, 0xEF);

        if (game.Call<State240>({em}) == 1) {
            if (Bits(0x2000) == 1)
                return Action(10, 0x9D);

            game.Call<0x80132370>({em, 0x10});
            return Action(10, 0x9E);
        }

        const uint32_t place = R8(em + kPlace);

        if (place == 1 || place == 3) {
            if (Bits(0x800) == 1)
                return Action(10, 0x83);

            game.Call<0x80121ABC>({em, 0x10});
            return 1;
        }

        if (place == 2)
            return Action(10, 0x8C);

        if (place == 4) {
            if (Bits(0x1000) == 1)
                return Action(10, 0x96);

            game.Call<0x80121BB8>({em, 0x10});
            return 1;
        }

        return ByPick(10, 9, 0x7E, 0x7F, 0x7D);
    }

    uint32_t Reaction9() const {
        if (game.Call<State934>({em}) == 1)
            return Action(10, 0xB7);

        if (game.Call<StateDBC>({em}) == 1)
            return ByKind(0xE4, 0xEA);

        if (game.Call<State240>({em}) == 1)
            return Action(10, 0xA7);

        const uint32_t place = R8(em + kPlace);

        if (place == 1) {
            if (R8(em + kLedge) == 2)
                return Action(10, 0xA4);

            return R8(em + kLedge) == 1 ? Action(10, 0xA5) : 0;
        }

        if (place == 3)
            return 0;

        return Action(10, place == 4 ? 0xA3 : place == 2 ? 0xA2 : 0xA0);
    }

    uint32_t Reaction6() const {
        if (game.Call<State934>({em}) == 1)
            return Action(11, 0x1C);

        const uint32_t place = R8(em + kPlace);

        if (place == 1 || place == 3)
            return 0;

        return Action(11, place == 2 ? 0x1B : 0x1A);
    }

    uint32_t Stagger() const {
        const uint32_t part = R8(em + kStagger);

        if (part >= 8)
            return 0;

        return Action(10, (R8(em + kPlace) == 2 ? 0xCB : 0xC3) + part);
    }

    uint32_t Reaction18() const {
        const uint32_t place = R8(em + kPlace);

        if (place != 0 && place != 4)
            return 0;

        const bool on = S16(em + kAt81E) >= 1;
        return Action(10, place == 0 ? (on ? 0xF4 : 0xF3) : (on ? 0xF6 : 0xF5));
    }

    uint32_t Reaction8() const {
        const uint32_t place = R8(em + kPlace);

        if (place != 0 && place != 2)
            return 0;

        return Action(10, place == 0 ? 0x78 : 0x79);
    }

    uint32_t Run() const {
        switch (R16(em + kReaction)) {
        case 1:
            return game.Call<0x8012072C>({em, 0});
        case 2:
            return Call(0x80120BE8);
        case 3:
            return Call(0x801219E0);
        case 4:
            return Call(0x80121BF8);
        case 5:
            return Call(0x80121DB4);
        case 6:
            return Reaction6();
        case 7:
            return Reaction7();
        case 8:
            return Reaction8();
        case 9:
            return Reaction9();
        case 10:
            return ByPlace(kReaction10);
        case 0xB:
            return ByPlace(kReaction11);
        case 0xC:
            game.Call<Mark7DC>({em, 1});
            return Action(10, 0xB6);
        case 0xD:
            game.Call<Mark7DC>({em, 1});
            return Action(10, R8(em + kPlace) == 2 ? 0xC0 : 0xBF);
        case 0xE:
            return Stagger();
        case 0xF:
            if (R8(em + kPlace) == 2)
                return Action(11, 0x13);

            return ByPick(11, 0xD, 0x11, 0x12, 0x10);
        case 0x10:
            return ByPick(10, 10, 0xD4, 0xD5, 0xD3);
        case 0x11:
            return Call(0x8012291C);
        case 0x12:
            return Call(0x80122CC8);
        case 0x13:
            return Call(0x80122D10);
        case 0x14:
            return Call(0x80122E0C);
        case 0x15:
            return Call(0x80122E38);
        case 0x16:
            return Call(0x80122EF0);
        case 0x17:
            return Call(0x80122F1C);
        case 0x18:
            return Reaction18();
        case 0x19:
            return ByPlace(kReaction19);
        case 0x1A:
            return Action(10, 0xF7);
        case 0x1B:
            return game.Call<0x8012072C>({em, 1});
        default:
            return 0;
        }
    }
};

uint32_t ReactionStart(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    return Start{game, frame.Arg(3)}.Run();
}

} // namespace

extern "C" void func_80123150(CpuContext* ctx);
extern "C" void MH3_EnemyReactionStart(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyReactionStart", func_80123150, [](CpuContext* c) {
        const uint32_t started = ReactionStart(c);
        c->gpr[3] = started;
    });
}
REGISTER_NATIVE_FUNCTION_AS(0x80123150, MH3_EnemyReactionStart, "EnemyReactionStart");
