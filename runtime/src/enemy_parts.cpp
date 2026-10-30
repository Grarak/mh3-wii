// Monster parts: damage, stages and breaks, RMHP08. C reconstructions from
// the disassembly (decompiled with Ghidra, whose calls lose register
// arguments; those come from the translated code).
//
// A monster has 8 parts, 6 bytes each in its work from +0x838:
//   +0x838 u8  the part's level: how often its hits counter ran out. A
//              level reached shows: see the stages below
//   +0x839 u8  how often its stagger counter ran out
//   +0x83A s16 hits left to the next level
//   +0x83C s16 stagger damage left
//
//   0x8011F6B8 EnemyDamageResolve(em): once a step, the step's damage per
//              part (s16 +0x7CA) off the parts' counters and the health, and
//              the reaction it asks for (u16 +0x82C): 0xE for part +0x832's
//              stagger, 1 or 0x11 at no health, ...
//   0x8011EAF4 EnemyPartStage(em, part): the part's new level. The species'
//              stage list (data +0x88, 0x20 bytes each, ended by part 0xFF):
//              u8 part, level, effect (0 or 1, kinds 0 and 2 of 0x80305988),
//              sound (0-3), id (bit 7: below), s32 joint +8 (-1 none), VEC3
//              offset +0xC, f32 scale +0x18, s32 effect +0x1C. An entry of
//              the level makes its effect and sound on the part's joint, and
//              breaks id & 0x7F (0x8011EA68) when every other entry of that
//              id has reached its level (bit 7), or when none has (no bit
//              7). Last the species' handler 0x14
//   0x8011EA68 EnemyPartBreak(em, bit): u8 +0x14's bit, once: the quest's
//              count of it (0x803A92A4) and, online, the others told
//   0x8011E83C, 0x8011E8FC  (em, part, n): adds to the level, the stagger count
//   0x8011EA40, 0x8011EA54  (em, part): the level, the stagger count
//   0x8011E858 (em, part): the hits counter anew, the species' value (data
//              +0xA0 -> +4, s16 per part) times the quest's factor
//              (0x801260A8 +0xC)
//   0x8011E918 (em, part): the stagger counter anew, the part's value
//              (0x80126914 +4), 1.5 times from level 41 (u8 +0x7C8)
//   0x8011E9C4 (em): all 8 parts anew
//   0x80126914 (em, part): the part's stagger record (data +0xA0 -> +0xC, 6
//              bytes each: u16 max count, u8 kinds[2], s16 damage), or the
//              empty one at sda -0x7448
//   0x801266F4 (em, kind): which of the species' two stagger kinds (data
//              +0xA0 -> +8, two bytes) `kind` is, -1 for neither; the step's
//              damage of each is at +0x7DA + kind * 0x10 + part * 2
//   0x8011E6A4 (em, bits): 1 when the reactions word u32 +0x824 has them all
//   0x8011E7C4 (em, health): sets the health, within 0, the maximum and the
//              floor (+0x7B0 of the maximum)
//
// How a part looks broken. Great Jaggi's frill (material 2, ba_0) is cut by
// an alpha map of three levels: 255 stays, 0 never shows, and the torn edges
// are 136. An alpha test of 60 shows them, the 140 of a broken frill does not:
//   0x80175E0C Em012PartAlpha(em): handler 8, each step after the species'
//              others (EnemyActiveStep): the test of material 2 by the level
//              of part 0, scaled by the fade (+0x1D4); not for kind 14
//   0x801541E8 EmPartAlpha(em): handler 8 of the wyverns' code (em001 and
//              em002 test 70 on the same two materials): 1 by part 1 (broken
//              160), 2 by part 2 (broken 120 up to 220)
//   0x801763D8 Em014PartStage(em, part): handler 0x14 of kinds 12 and 14: at
//              kind 14's first head level, 0x803B9C04(em, position, 100)
//   0x8012E924 (em): 2 when u8 +0x14 is 2, else 0
// MonsterColours (0x80137434) sets every material back to the resource's
// earlier in the step, so the live fields hold the unbroken test between the
// two: a draw must not read them there (HoldForDraw, model_catalog.cpp).
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"

#include <cstdint>

namespace {

enum : uint32_t {
    kBroken = 0x14,        // u8: the broken stages' bits
    kKind = 0x03,          // u8
    kId = 0x1A,            // u16: the quest's monster
    kFlags = 0x1C8,        // u32: 1 a boss's health, 8 online
    kHalted = 0x1F8,       // u8: no damage while set
    kPlace = 0x1E2,        // u8: 0 land, 2 water
    kMode = 0x1E5,         // u8: the action's mode
    kFade = 0x1D4,         // f32
    kHealth = 0x7A0,       // s32
    kMaxHealth = 0x7A4,    // s32
    kHealthFloor = 0x7B0,  // f32
    kLevel = 0x7C8,        // u8: the monster's
    kPartDamage = 0x7CA,   // s16[8]: the step's
    kKindDamage = 0x7DA,   // s16[2][8]
    kAt800 = 0x800,        // u8
    kAt801 = 0x801,        // u8
    kReactBits = 0x80E,    // u16
    kLock81A = 0x81A,      // s16
    kLock81C = 0x81C,      // s16
    kStaggerMask = 0x820,  // u8: parts that cannot stagger
    kStaggerLock = 0x821,  // s8
    kReactions = 0x824,    // u32: the reactions this monster has
    kReactionNow = 0x82A,  // s16
    kReaction = 0x82C,     // u16: asked
    kLevelled = 0x82E,     // u16: the parts whose level rose this step
    kHardestPart = 0x831,  // u8
    kStaggerPart = 0x832,  // s8: -1 none
    kReacting = 0x833,     // u8
    kArmed = 0x834,        // u8
    kArmKind = 0x835,      // u8
    kParts = 0x838,        // 6 bytes each
    kOnce = 0x918,         // u8: 1 once, cleared here, for reaction 10
    kFixedDamage = 0x928,  // s16
};

enum : uint32_t {
    Alive = 0x8012D204,
    Dead = 0x8012D090,
    EnemyData = 0x80126098,
    QuestFactors = 0x801260A8,
    Handler = 0x80126520,
    AddHealth = 0x8011E748,
    RageAdd = 0x801308BC,
    AfterDamage = 0x80123150,
};

uint32_t R8(uint32_t a) {
    return MemoryInline::FlatRead8(a);
}

uint32_t R16(uint32_t a) {
    return MemoryInline::FlatRead16(a);
}

int32_t S16(uint32_t a) {
    return int16_t(MemoryInline::FlatRead16(a));
}

int32_t S8(uint32_t a) {
    return int8_t(MemoryInline::FlatRead8(a));
}

uint32_t R32(uint32_t a) {
    return MemoryInline::FlatRead32(a);
}

float RF(uint32_t a) {
    return MemoryInline::FlatReadFloat32(a);
}

void W8(uint32_t a, uint32_t v) {
    MemoryInline::FlatWrite8(a, uint8_t(v));
}

void W16(uint32_t a, uint32_t v) {
    MemoryInline::FlatWrite16(a, uint16_t(v));
}

void W32(uint32_t a, uint32_t v) {
    MemoryInline::FlatWrite32(a, v);
}

uint32_t PartAt(uint32_t em, uint32_t part) {
    return em + (part & 0xFF) * 6 + kParts;
}

uint32_t HasReactions(uint32_t em, uint32_t bits) {
    return (bits & R32(em + kReactions)) == bits;
}

void SetHealth(uint32_t em, int32_t health) {
    if (health < 0) {
        W32(em + kHealth, 0);
        return;
    }

    const int32_t max = int32_t(R32(em + kMaxHealth));

    if (max < health) {
        W32(em + kHealth, uint32_t(max));
        return;
    }

    const int32_t floor = int32_t(float(max) * RF(em + kHealthFloor));
    W32(em + kHealth, uint32_t(health < floor ? floor : health));
}

void BreakStage(CpuContext* ctx, uint32_t em, uint32_t bit) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const uint32_t mask = 1u << (bit & 0xFF);

    if (uint32_t(S8(em + kBroken)) & mask)
        return;

    game.Call<0x803A92A4>({R8(em + kKind), R16(em + kId), bit & 0xFF});
    W8(em + kBroken, R8(em + kBroken) | mask);

    if (R32(em + kFlags) & 8)
        game.Call<0x803373E0>({em, 5, 2});
}

void ResetHits(CpuContext* ctx, uint32_t em, uint32_t part) {
    GuestFrame frame(ctx, 0x30);
    GuestCall game{ctx};
    const float factor = RF(game.Call<QuestFactors>({em}) + 0xC);
    const uint32_t table = R32(R32(game.Call<EnemyData>({em}) + 0xA0) + 4);
    const uint32_t i = part & 0xFF;
    W16(PartAt(em, i) + 2, uint32_t(int32_t(factor * float(S16(table + i * 2)))));
}

uint32_t StaggerRecord(CpuContext* ctx, uint32_t em, uint32_t part) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const uint32_t list = R32(R32(game.Call<EnemyData>({em}) + 0xA0) + 0xC);
    return list ? list + (part & 0xFF) * 6 : frame.Arg(13) - 0x7448;
}

void ResetStagger(CpuContext* ctx, uint32_t em, uint32_t part) {
    GuestFrame frame(ctx, 0x30);
    GuestCall game{ctx};
    const float scale = R8(em + kLevel) <= 40 ? 1.0f : 1.5f;
    const uint32_t record = game.Call<0x80126914>({em, part & 0xFF});
    W16(PartAt(em, part) + 4, uint32_t(int32_t(scale * float(S16(record + 4)))));
}

void ResetParts(CpuContext* ctx, uint32_t em) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};

    for (uint32_t i = 0; i < 8; ++i) {
        const uint32_t at = PartAt(em, i);
        W8(at, 0);
        game.Call<0x8011E858>({em, i});
        W8(at + 1, 0);
        game.Call<0x8011E918>({em, i});
    }
}

int32_t StaggerKind(CpuContext* ctx, uint32_t em, uint32_t kind) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const uint32_t kinds = R32(R32(game.Call<EnemyData>({em}) + 0xA0) + 8);

    if (kinds) {
        if (uint32_t(S8(kinds)) == (kind & 0xFF))
            return 0;

        if (uint32_t(S8(kinds + 1)) == (kind & 0xFF))
            return 1;
    }

    return -1;
}

// The first stage of `part` at its level, its effect and sound made.
uint32_t StageEffects(GuestCall& game, GuestFrame& frame, uint32_t em, uint32_t part, uint32_t stages) {
    const uint32_t mtx = frame[0x20], at = frame[0x14], offset = frame[8];

    for (uint32_t e = stages; R8(e) != 0xFF; e += 0x20) {
        if (R8(e) != part || R8(e + 1) != R8(PartAt(em, part)))
            continue;

        const uint32_t joint = R32(e + 8);

        if (game.Call<0x8012D0D8>({em}) != 1 || joint == 0xFFFFFFFFu)
            return e;

        const uint32_t kind = R8(e + 2), sound = R8(e + 3);
        game.Call<0x80041EF0>({offset}, {RF(e + 0xC), RF(e + 0x10), RF(e + 0x14)});
        game.Call<0x80041EA4>({at, offset});

        if (kind <= 1)
            game.Call<0x80305988>({em, kind * 2, joint, at, R32(e + 0x1C)}, {RF(e + 0x18)});

        if (sound < 4) {
            static constexpr uint32_t kSounds[]{0x24, 0x40, 0x41, 0x42};
            game.Call<0x80135994>({em, joint, mtx});
            game.Call<0x80051510>({at, mtx});
            game.Call<0x800D9F0C>({R32(em + 0xB14), kSounds[sound], 2, at});
        }

        return e;
    }

    return 0;
}

void PartStage(CpuContext* ctx, uint32_t em, uint32_t part) {
    GuestFrame frame(ctx, 0x70);
    GuestCall game{ctx};
    part &= 0xFF;

    if (const uint32_t stages = R32(game.Call<EnemyData>({em}) + 0x88)) {
        if (const uint32_t stage = StageEffects(game, frame, em, part, stages)) {
            const uint32_t id = R8(stage + 4);
            const bool all = id & 0x80;
            bool breaks = true;

            for (uint32_t e = stages; R8(e) != 0xFF; e += 0x20) {
                if (e == stage || R8(e + 4) != id)
                    continue;

                if ((R8(PartAt(em, R8(e))) >= R8(e + 1)) != all) {
                    breaks = false;
                    break;
                }
            }

            if (breaks)
                game.Call<0x8011EA68>({em, id & 0x7F});
        }
    }

    if (const uint32_t handler = game.Call<Handler>({em, 0x14}))
        game(handler, {em, part});
}

// The stagger damage of a part's two kinds: the one given, or the larger.
int32_t StaggerDamage(GuestCall& game, uint32_t em, uint32_t part, uint32_t record) {
    int32_t damage[2]{-1, -1};

    for (uint32_t i = 0; i < 2; ++i)
        if (const uint32_t kind = R8(record + 2 + i); kind != 0xFF)
            if (const int32_t k = int32_t(game.Call<0x801266F4>({em, kind})); k != -1)
                damage[i] = S16(uint32_t(k * 0x10) + part * 2 + em + kKindDamage);

    const int32_t a = damage[0], b = damage[1];

    if (a > 0 && b == -1)
        return a;

    if (b > 0 && a == -1)
        return b;

    return a > 0 && b > 0 ? (a > b ? a : b) : 0;
}

struct Resolve {
    GuestCall& game;
    uint32_t em;
    int32_t total = 0;
    bool once = false;  // u8 +0x918 was 1
    bool armed = false; // +0x834 and reaction 0x10000
    bool quiet = false; // no reaction from here (0x8011F584 said no, no damage)
    int32_t condition = 0;

    uint32_t Bits(uint32_t bits) const { return HasReactions(em, bits); }
    uint32_t Call(uint32_t f) const { return game(f, {em}); }
    void Ask(uint32_t reaction) const { W16(em + kReaction, reaction); }

    void ArmFall() {
        if (!(R16(em + kReactBits) & 1))
            return;

        if (R8(em + kArmed) == 1 && Bits(0x10000)) {
            armed = true;
            return;
        }

        if (Bits(0x4000) != 1 || R8(em + kArmKind) == 0 || (R8(em + kArmKind) == 1 && game.Call<0x8012E4C8>({em, 0}) == 1))
            W8(em + kArmed, 1);
    }

    void Parts() {
        int32_t hardest = 0;

        for (uint32_t part = 0; part < 8; ++part) {
            const int32_t damage = S16(em + kPartDamage + part * 2);
            const uint32_t at = PartAt(em, part);
            const int32_t hits = int16_t(S16(at + 2) - damage);
            total += damage;

            if (hits < 1) {
                W16(em + kLevelled, R16(em + kLevelled) | (1u << part));
                game.Call<0x8011E83C>({em, part, 1});
                game.Call<0x8011E858>({em, part});
                game.Call<0x8011EAF4>({em, part, 0});

                if (int16_t(hardest) < damage) {
                    W8(em + kHardestPart, part);
                    hardest = damage;
                }
            } else
                W16(at + 2, uint32_t(hits));

            Stagger(part, at);
        }
    }

    void Stagger(uint32_t part, uint32_t at) const {
        if ((R8(em + kStaggerMask) & (1u << part)) && S8(em + kStaggerLock) >= 1)
            return;

        const uint32_t record = game.Call<0x80126914>({em, part});

        if (R8(at + 1) >= R16(record))
            return;

        const int32_t damage = StaggerDamage(game, em, part, record);

        if (damage <= 0)
            return;

        const int32_t left = int16_t(S16(at + 4) - damage);

        if (left >= 1) {
            W16(at + 4, uint32_t(left));
            return;
        }

        if (Call(0x8012D934) == 0 && Call(0x8012DDBC) == 0 && Call(0x8011F484) == 1 && Call(0x80132240) == 0 && (R8(em + kPlace) == 0 || R8(em + kPlace) == 2) &&
            S8(em + kStaggerPart) == -1)
            W8(em + kStaggerPart, part);

        W16(at + 4, 1);
    }

    // The asks that end the step before its health is taken.
    bool Taken() const {
        if (!(S16(em + kLock81A) < 1 && Call(0x8012F040) == 1 && Call(0x80132240) == 0))
            return false;

        if (condition == 3) {
            Ask(6);
            return true;
        }

        if (R8(em + kReacting) != 0 && S16(em + kReactionNow) == 0xC && Bits(0x80) == 1 && (R8(em + kPlace) == 0 || R8(em + kPlace) == 2)) {
            game.Call<0x8011E7C4>({em, 0});
            Ask(0xF);
            return true;
        }

        if (S8(em + kStaggerPart) != -1 && Bits(0x8000) == 1) {
            game.Call<0x8011E7C4>({em, 0});
            Ask(0x14);
            W8(em + kAt801, R8(em + kAt800));
            return true;
        }

        if (armed) {
            game.Call<0x8011E7C4>({em, 0});
            Ask(0x15);
            return true;
        }

        return false;
    }

    // The health taken. True when it killed the monster, the reaction asked.
    bool Health() const {
        const int32_t fixed = S16(em + kFixedDamage);

        if (fixed != 0) {
            game.Call<AddHealth>({em, uint32_t(-fixed)});

            if ((R32(em + kFlags) & 1) && int32_t(R32(em + kHealth)) < 1)
                game.Call<0x8011E7C4>({em, 1});
        }

        if (total != 0)
            game.Call<AddHealth>({em, uint32_t(-total)});

        if ((total != 0 || fixed != 0) && int32_t(R32(em + kHealth)) < 1) {
            if (S16(em + kLock81A) < 1 && Call(0x8012F040) != 0) {
                Ask(fixed == 0 || Bits(0x200) != 1 ? 1 : 0x11);
                return true;
            }

            game.Call<0x8011E7C4>({em, 1});
        }

        return false;
    }

    // During reaction +0x82A: its follow-up. False when it has none.
    bool FollowUp() const {
        const int32_t now = S16(em + kReactionNow);

        if (now == 10) {
            if (Bits(0x20) != 1)
                return false;

            Ask(0xC);
            return true;
        }

        if (now == 0xB) {
            if (Call(0x8012DBA0) != 0 || Bits(0x40) != 1)
                return false;

            Ask(0xD);
            return true;
        }

        if (now == 0x22) {
            if (Call(0x8012DEDC) != 0 || Bits(0x40000) != 1)
                return false;

            Ask(0x17);
            return true;
        }

        if (now == 0x23) {
            if (Call(0x8012DF3C) != 0 || Bits(0x80000) != 1)
                return false;

            Ask(0x18);
            return true;
        }

        if (now != 0x10 || Call(0x8012E5AC) != 0 || Bits(0x400) != 1)
            return false;

        Ask(0x12);
        return true;
    }

    void Flinch() {
        const uint32_t mode = R8(em + kMode);
        const uint32_t can = R16(em + kReactBits);

        if (R8(em + kReacting) != 0 && ((R16(em + kReactionNow) - 4u) < 2 || (R16(em + kReactionNow) - 0x23u) < 2) && Bits(0x10) == 1) {
            if (Call(0x8011F584) == 1) {
                Ask(0xB);
                return;
            }

            if (total == 0)
                quiet = true;
        }

        if (once && Bits(4) == 1) {
            Ask(10);
            return;
        }

        if (condition == 5 && Call(0x80132240) == 0 && (mode != 10 || Call(0x8012D8B4) == 1)) {
            Ask(8);
            return;
        }

        if (!((mode == 10 && Call(0x8012D8B4) != 1) || R16(em + kLevelled) == 0 || Bits(2) != 1)) {
            Ask(3);
            return;
        }

        if (uint8_t(mode - 8) < 2 && S16(em + kLock81C) > 0 && (can & 2)) {
            Ask(0x16);
            return;
        }

        if (!(mode == 10 || !(can & 2) || Bits(0x100000) != 1)) {
            Ask(0x19);
            return;
        }

        if (quiet)
            return;

        if (!(mode == 10 || R8(em + kPlace) != 0 || Call(0x80132240) != 0 || Bits(0x100) != 1 || Call(0x8011F534) != 1)) {
            Ask(0x10);
            return;
        }

        if (!(mode == 10 || Call(0x8011F5D4) != 1 || Bits(0x4000) != 1 || !(can & 1))) {
            Ask(0x13);
            return;
        }

        if ((mode != 10 || game.Call<0x8012E4C8>({em, 0}) == 1) && (Call(0x80132240) == 0 || Bits(0x200000) == 1) && Bits(1) == 1)
            Ask(2);
    }

    void Reaction() {
        if (Call(0x8012DF3C) == 1 || Call(0x8012E5AC) == 1) {
            Ask(0);
            return;
        }

        const float left = float(int32_t(R32(em + kHealth))) / float(int32_t(R32(em + kMaxHealth)));

        if (!(RF(em + kHealthFloor) < left) && Bits(0x400000) == 1) {
            Ask(0x1A);
            return;
        }

        if (S8(em + kStaggerPart) != -1) {
            game.Call<0x8011E8FC>({em, uint32_t(S8(em + kStaggerPart)), 1});
            game.Call<0x8011E918>({em, R8(em + kStaggerPart)});
            Ask(0xE);
            return;
        }

        if (R8(em + kReacting) != 0 && game.Call<0x8012F368>({em, 1}) == 1) {
            Ask(9);
            return;
        }

        if (game.Call<0x80132330>({em, 0x1D}) != 0 || Call(0x8012D908) != 0)
            return;

        if (condition == 1)
            Ask(4);
        else if (condition == 2)
            Ask(5);
        else if (condition == 4)
            Ask(7);
        else if ((condition == 0 || condition == 5) && Call(0x8012D934) == 0 && Call(0x8012DBA0) == 0 && Call(0x8012DDBC) == 0 && Call(0x8012DFCC) == 0)
            Flinch();
    }

    // Reactions and the step's rage. Before the health is taken nothing asks.
    void React() {
        if (S16(em + kLock81A) < 1 && Call(0x8012DEDC) != 1) {
            if (R8(em + kReacting) != 0 || once || condition != 0) {
                if (R8(em + kReacting) == 0 || Call(0x8012D934) != 0 || !FollowUp())
                    Reaction();
            }
        } else
            Ask(0);

        if (total != 0)
            game.Call<RageAdd>({em, uint32_t(int32_t(int16_t(game.Call<0x80120510>({em, uint32_t(total)}))))});
    }

    void Run() {
        ArmFall();
        Parts();
        condition = int8_t(Call(0x801201FC));

        if (Taken())
            return;

        if (Health())
            return;

        W16(em + kFixedDamage, 0);
        React();
    }
};

uint32_t DamageResolve(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x40);
    GuestCall game{ctx};
    const uint32_t em = frame.Arg(3);
    Resolve r{game, em};
    r.once = R8(em + kOnce) == 1;

    if (r.once)
        W8(em + kOnce, 0);

    if (game.Call<Alive>({em}) == 0 || R8(em + kHalted) != 0)
        return 0;

    W16(em + kLevelled, 0);
    W16(em + kReaction, 0);
    W8(em + kStaggerPart, 0xFF);

    if (game.Call<Dead>({em}) == 0)
        r.Run();

    return game.Call<AfterDamage>({em});
}

// Handler 8s: an alpha test of a material, by a part's level, faded.
void AlphaTest(GuestCall& game, uint32_t em, uint32_t material, uint32_t reference0, uint32_t reference1) {
    const float fade = RF(em + kFade);
    game.Call<0x800E2FA4>({em + 0x24, material, 6, uint32_t(int32_t(float(reference0) * fade)) & 0xFF, 0, 3, uint32_t(int32_t(float(reference1) * fade)) & 0xFF});
}

void Em012PartAlpha(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const uint32_t em = frame.Arg(3);

    if (R8(em + kKind) != 14)
        AlphaTest(game, em, 2, R8(em + kParts) == 0 ? 60 : 140, 255);
}

void EmPartAlpha(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const uint32_t em = frame.Arg(3);
    AlphaTest(game, em, 1, R8(PartAt(em, 1)) == 0 ? 70 : 160, 255);
    const bool broken = R8(PartAt(em, 2)) != 0;
    AlphaTest(game, em, 2, broken ? 120 : 70, broken ? 220 : 255);
}

void Em014PartStage(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const uint32_t em = frame.Arg(3);

    if ((frame.Arg(4) & 0xFF) == 0 && R8(em + kKind) == 14 && R8(em + kParts) == 1)
        game.Call<0x803B9C04>({em, em + 0x188, 100});
}

} // namespace

extern "C" void func_8011F6B8(CpuContext* ctx);
extern "C" void MH3_EnemyDamageResolve(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyDamageResolve", func_8011F6B8, [](CpuContext* c) {
        const uint32_t result = DamageResolve(c);
        c->gpr[3] = result;
    });
}
REGISTER_NATIVE_FUNCTION_AS(0x8011F6B8, MH3_EnemyDamageResolve, "EnemyDamageResolve");

extern "C" void func_8011EAF4(CpuContext* ctx);
extern "C" void MH3_EnemyPartStage(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyPartStage", func_8011EAF4, [](CpuContext* c) { PartStage(c, c->gpr[3], c->gpr[4]); });
}
REGISTER_NATIVE_FUNCTION_AS(0x8011EAF4, MH3_EnemyPartStage, "EnemyPartStage");

extern "C" void func_8011EA68(CpuContext* ctx);
extern "C" void MH3_EnemyPartBreak(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyPartBreak", func_8011EA68, [](CpuContext* c) { BreakStage(c, c->gpr[3], c->gpr[4]); });
}
REGISTER_NATIVE_FUNCTION_AS(0x8011EA68, MH3_EnemyPartBreak, "EnemyPartBreak");

extern "C" void func_8011E858(CpuContext* ctx);
extern "C" void MH3_EnemyPartResetHits(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyPartResetHits", func_8011E858, [](CpuContext* c) { ResetHits(c, c->gpr[3], c->gpr[4]); });
}
REGISTER_NATIVE_FUNCTION_AS(0x8011E858, MH3_EnemyPartResetHits, "EnemyPartResetHits");

extern "C" void func_8011E918(CpuContext* ctx);
extern "C" void MH3_EnemyPartResetStagger(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyPartResetStagger", func_8011E918, [](CpuContext* c) { ResetStagger(c, c->gpr[3], c->gpr[4]); });
}
REGISTER_NATIVE_FUNCTION_AS(0x8011E918, MH3_EnemyPartResetStagger, "EnemyPartResetStagger");

extern "C" void func_8011E9C4(CpuContext* ctx);
extern "C" void MH3_EnemyPartsReset(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyPartsReset", func_8011E9C4, [](CpuContext* c) { ResetParts(c, c->gpr[3]); });
}
REGISTER_NATIVE_FUNCTION_AS(0x8011E9C4, MH3_EnemyPartsReset, "EnemyPartsReset");

extern "C" void func_80126914(CpuContext* ctx);
extern "C" void MH3_EnemyStaggerRecord(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyStaggerRecord", func_80126914, [](CpuContext* c) {
        const uint32_t record = StaggerRecord(c, c->gpr[3], c->gpr[4]);
        c->gpr[3] = record;
    });
}
REGISTER_NATIVE_FUNCTION_AS(0x80126914, MH3_EnemyStaggerRecord, "EnemyStaggerRecord");

extern "C" void func_801266F4(CpuContext* ctx);
extern "C" void MH3_EnemyStaggerKind(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyStaggerKind", func_801266F4, [](CpuContext* c) {
        const int32_t kind = StaggerKind(c, c->gpr[3], c->gpr[4]);
        c->gpr[3] = uint32_t(kind);
    });
}
REGISTER_NATIVE_FUNCTION_AS(0x801266F4, MH3_EnemyStaggerKind, "EnemyStaggerKind");

extern "C" void MH3_EnemyPartLevelAdd(CpuContext* ctx) {
    const uint32_t at = PartAt(ctx->gpr[3], ctx->gpr[4]);
    W8(at, R8(at) + ctx->gpr[5]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8011E83C, MH3_EnemyPartLevelAdd, "EnemyPartLevelAdd");

extern "C" void MH3_EnemyPartStaggerAdd(CpuContext* ctx) {
    const uint32_t at = PartAt(ctx->gpr[3], ctx->gpr[4]) + 1;
    W8(at, R8(at) + ctx->gpr[5]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8011E8FC, MH3_EnemyPartStaggerAdd, "EnemyPartStaggerAdd");

extern "C" void MH3_EnemyPartLevel(CpuContext* ctx) {
    ctx->gpr[3] = R8(PartAt(ctx->gpr[3], ctx->gpr[4]));
}
REGISTER_NATIVE_FUNCTION_AS(0x8011EA40, MH3_EnemyPartLevel, "EnemyPartLevel");

extern "C" void MH3_EnemyPartStagger(CpuContext* ctx) {
    ctx->gpr[3] = R8(PartAt(ctx->gpr[3], ctx->gpr[4]) + 1);
}
REGISTER_NATIVE_FUNCTION_AS(0x8011EA54, MH3_EnemyPartStagger, "EnemyPartStagger");

extern "C" void MH3_EnemyHasReactions(CpuContext* ctx) {
    ctx->gpr[3] = HasReactions(ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8011E6A4, MH3_EnemyHasReactions, "EnemyHasReactions");

extern "C" void MH3_EnemySetHealth(CpuContext* ctx) {
    SetHealth(ctx->gpr[3], int32_t(ctx->gpr[4]));
}
REGISTER_NATIVE_FUNCTION_AS(0x8011E7C4, MH3_EnemySetHealth, "EnemySetHealth");

extern "C" void MH3_EnemyBrokenStageTwo(CpuContext* ctx) {
    ctx->gpr[3] = R8(ctx->gpr[3] + kBroken) == 2 ? 2 : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x8012E924, MH3_EnemyBrokenStageTwo, "EnemyBrokenStageTwo");

extern "C" void func_80175E0C(CpuContext* ctx);
extern "C" void MH3_Em012PartAlpha(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em012PartAlpha", func_80175E0C, Em012PartAlpha);
}
REGISTER_NATIVE_FUNCTION_AS(0x80175E0C, MH3_Em012PartAlpha, "Em012PartAlpha");

extern "C" void func_801541E8(CpuContext* ctx);
extern "C" void MH3_EmPartAlpha(CpuContext* ctx) {
    PortCheck::Call(ctx, "EmPartAlpha", func_801541E8, EmPartAlpha);
}
REGISTER_NATIVE_FUNCTION_AS(0x801541E8, MH3_EmPartAlpha, "EmPartAlpha");

extern "C" void func_801763D8(CpuContext* ctx);
extern "C" void MH3_Em014PartStage(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em014PartStage", func_801763D8, Em014PartStage);
}
REGISTER_NATIVE_FUNCTION_AS(0x801763D8, MH3_Em014PartStage, "Em014PartStage");
