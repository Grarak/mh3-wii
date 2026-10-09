// Player step timers, RMHP08. C reconstruction of the player's per-step
// bookkeeping (0x80260B9C, called from 0x802095A8 once per step with the PLW),
// decompiled with Ghidra (scripts/dol2elf.py -> ELF import).
//
// Everything here counts steps: the player's own step counters (+0x20, +0x24,
// +0x30), some forty countdowns (invulnerability, item and skill effects,
// status conditions), the stamina recovery at +0x264, and periodic events on
// the step counter +0x20 (health recovery every N steps, hot/cold zones every
// 128). The body therefore runs once per whole tick of game time elapsed this
// step (GameStep::Ticks: none on a step shorter than a tick, several on a long
// one), so all of it keeps the Wii's rate at any step length. At the stock
// 1/30 s step it runs once, as the original.
//
// Cheats (cheats.h), by the instruction they replaced: Super speed sets the
// motion speed (+0x354) to 5 before the stamina recovery reads it; On hit
// long sword power max stores 100 to the spirit gauge and its level (+0x384,
// +0x386) where the gauge would go down.
#include "cheats.h"
#include "game_step.h"
#include "guest_call.h"
#include "port_check.h"

#include <cstdint>
#include <tuple>
#include <utility>

namespace {

enum : uint32_t {
    MasterCheck = 0x8026FE38,       // Pl_master_ck(PLW*) -> 1 for the local player
    SkillCheck = 0x80271374,        // Pl_Skill_ck(PLW*, skill) -> 1
    DamageCondition = 0x80279130,   // Pl_dm_condition_ck(PLW*, condition) -> 1
    OnWaterSurface = 0x80278878,    // Pl_suimen_ck(PLW*)
    SetEffect013 = 0x801072B4,      // eft013_set(PLW*, u8)
    InEventDemo = 0x8027BCAC,       // (1) -> 0 when messages may be shown
    ShowStatusMessage = 0x802672D4, // (PLW*, kind, message, arg)
    AreaCondition = 0x80331190,     // (PLW*) -> 1
    AreaConditionTick = 0x8027D608, // (PLW*, 5)
    PendingStatus = 0x8027BD44,     // (PLW*) -> u16 status, 0xFFFF none
    ApplyStatus = 0x80272E18,       // (PLW*, u16 status)
    ConditionActive = 0x802785DC,   // (PLW*, 1) -> 1
    ConditionActiveB = 0x80276880,  // (PLW*) -> 1
    SetConditionState = 0x802768CC, // (PLW*, state)
    ClimateDamage = 0x80276818,     // (PLW*, amount; -1 = per tick damage)
    ClimateActive = 0x80276864,     // (PLW*, 0) -> 1
    ClimateTick = 0x80276514,       // (PLW*, -1, 0)
    ArmorLevel = 0x8027492C,        // (PLW*) -> u8
    MapClimate = 0x802B05FC,        // (u8 map) -> 1 hot, 3 cold, 4 ...
    StatusUpdate = 0x80276E6C,      // (PLW*)
    CanRecoverHealth = 0x80260ABC,  // (PLW*) -> 1
    RecoverHealth = 0x802762F0,     // (PLW*, amount)
};

struct Plw {
    uint32_t base;
    int8_t S8(uint32_t o) const { return int8_t(Memory::Read8(base + o)); }
    uint8_t U8(uint32_t o) const { return Memory::Read8(base + o); }
    int16_t S16(uint32_t o) const { return int16_t(Memory::Read16(base + o)); }
    uint16_t U16(uint32_t o) const { return Memory::Read16(base + o); }
    int32_t S32(uint32_t o) const { return int32_t(Memory::Read32(base + o)); }
    void W8(uint32_t o, uint32_t v) const { Memory::Write8(base + o, uint8_t(v)); }
    void W16(uint32_t o, uint32_t v) const { Memory::Write16(base + o, uint16_t(v)); }
    void W32(uint32_t o, uint32_t v) const { Memory::Write32(base + o, v); }
    // `if (x > 0) --x`, s8 / s16.
    void Down8(uint32_t o) const {
        if (S8(o) > 0)
            W8(o, U8(o) - 1);
    }

    void Down16(uint32_t o) const {
        if (S16(o) > 0)
            W16(o, U16(o) - 1);
    }

    // `if (x > 0 && --x < 1)` (s16): true when the countdown ran out now.
    bool Expired16(uint32_t o) const {
        if (S16(o) <= 0)
            return false;

        W16(o, U16(o) - 1);
        return S16(o) < 1;
    }
};

void Tick(GuestCall& game, const Plw p) {
    const uint32_t pl = p.base;
    p.W32(0x20, p.S32(0x20) + 1);
    p.W32(0x24, p.S32(0x24) + 1);
    p.W16(0x30, p.U16(0x30) + 1);

    if (p.S16(0x30) > 1000)
        p.W16(0x30, 1000);

    p.Down8(0x19);
    p.Down16(0x580);

    if (p.S8(0x3A2) > 0)
        p.W8(0x3A2, p.U8(0x3A2) - 1);
    else
        p.Down8(0x3A3);

    p.Down8(0x266);
    p.Down16(0x396);
    p.Down16(0x45A);
    p.Down16(0x45C);

    if (p.S16(0x264) < 30000) { // stamina recovery: 2 * rate per step, up to 30000
        if (Cheats::Toggled(Cheats::Id::SuperSpeed))
            Memory::WriteFloat32(pl + 0x354, 5.0f);

        const int16_t rate = int16_t(int32_t(Memory::ReadFloat32(pl + 0x354)));
        p.W16(0x264, uint16_t(p.S16(0x264) + int16_t(rate * 2)));

        if (p.S16(0x264) >= 30000)
            p.W16(0x264, 30000);
    }

    p.Down16(0x662);
    p.Down16(0x660);
    p.Down16(0x442);

    if (game.Call<MasterCheck>({pl}) == 0) {
        // Another player: only its leave countdown.
        if (p.U8(0x584) == 0)
            return;

        p.W16(0x256, p.U16(0x256) - 1);

        if (p.S16(0x256) > 0)
            return;

        p.W8(0, 0);
        p.W8(1, 0);
        return;
    }

    p.Down16(0x468);
    p.Down16(0x12);
    p.Down8(0x313);
    p.Down8(0x314);

    if (p.U8(0x658) != 0)
        p.W8(0x658, p.U8(0x658) - 1);

    p.Down8(0x36A);
    p.Down16(0x352);

    if (p.U16(0x59A) != 0)
        p.W16(0x59A, p.U16(0x59A) - 1);

    if (p.U8(0x668) != 0)
        p.W8(0x668, p.U8(0x668) - 1);

    p.Down16(0x66A);
    p.Down16(0x470);
    p.Down16(0x460);

    if (game.Call<AreaCondition>({pl}) == 1) {
        p.W16(0x36E, p.U16(0x36E) - 1);

        if (p.S16(0x36E) < 1) {
            p.W16(0x36E, 150);
            game.Call<AreaConditionTick>({pl, 5});
        }
    }

    const uint8_t delay = p.U8(0x5C4);

    if ((delay & 0xF) != 0 && p.U8(10) == 12) {
        p.W8(0x5C4, ((delay & 0xF) - 1) | (delay & 0xF0));
    } else {
        bool check = p.U8(10) != 0;

        if (!check) {
            const uint32_t action = p.U16(0xC);
            check = action - 49 > 3 && action - 97 > 2 && (action < 44 || action > 45);
        }

        if (check) {
            const uint16_t status = uint16_t(game.Call<PendingStatus>({pl}));

            if (status != 0xFFFF) {
                if (game.Call<MasterCheck>({pl}) == 1 && game.Call<InEventDemo>({1}) == 0)
                    game.Call<ShowStatusMessage>({pl, 2, 18, status});

                game.Call<ApplyStatus>({pl, status});
            }
        }
    }

    if (game.Call<ConditionActive>({pl, 1}) == 1 || game.Call<ConditionActiveB>({pl}) == 1) {
        if (game.Call<DamageCondition>({pl, 12}) == 1) {
            if (game.Call<ConditionActiveB>({pl}) == 1) {
                if (p.S16(0x412) > 1)
                    p.W16(0x412, p.U16(0x412) - 1);

                game.Call<SetConditionState>({pl, 4});
            } else {
                game.Call<SetConditionState>({pl, 1});
            }
        } else {
            game.Call<SetConditionState>({pl, 4});
        }
    }

    if (p.S16(0x412) > 0) {
        p.W16(0x412, p.U16(0x412) - 1);

        if (p.S16(0x412) == 0 && game.Call<InEventDemo>({1}) == 0)
            game.Call<ShowStatusMessage>({pl, 1, 24, 0});
    }

    if (p.U8(9) == 3) {
        if (p.U8(10) != 8) { // swimming
            if (game.Call<OnWaterSurface>({pl}) == 0) {
                if (p.S16(0x382) < 90) {
                    p.W16(0x382, p.U16(0x382) + 1);
                } else {
                    int32_t every = 128;

                    if (game.Call<SkillCheck>({pl, 193}) == 1)
                        every = 256;
                    else if (game.Call<SkillCheck>({pl, 194}) == 1)
                        every = 103;

                    if (p.S32(0x20) % every == 0) {
                        game.Call<ClimateDamage>({pl, 0xFFFFFFFFu});

                        if (p.S16(0x37E) == 25)
                            game.Call<SetEffect013>({pl, 3});
                    }
                }
            }

            if (game.Call<ClimateActive>({pl, 0}) == 1) {
                p.W32(0x3DC, uint32_t(p.S32(0x3DC)) | 0x100000);

                if ((p.S32(0x20) & 0xF) == 0)
                    game.Call<ClimateTick>({pl, 0xFFFFFFFFu, 0});
            }
        }
    } else if (p.U8(10) != 8) {
        game.Call<ClimateDamage>({pl, 150});
    }

    if (p.S16(0x44A) > 0) {
        p.W16(0x44A, p.U16(0x44A) - 1);

        if (p.S16(0x44A) == 0)
            p.W8(0x449, 0);
    }

    if (p.S16(0x44E) > 0) {
        p.W16(0x44E, p.U16(0x44E) - 1);

        if (p.S16(0x44E) == 0)
            p.W8(0x44D, 0);
    }

    if (p.Expired16(0x406))
        p.W32(0x3DC, uint32_t(p.S32(0x3DC)) & 0xFFFFFFF3u);

    if (p.Expired16(0x408))
        p.W32(0x3DC, uint32_t(p.S32(0x3DC)) & 0xFFFFFFCFu);

    if (p.Expired16(0x40A))
        p.W32(0x3DC, uint32_t(p.S32(0x3DC)) & 0xFFFFFF3Fu);

    if (p.Expired16(0x40C))
        p.W32(0x3DC, uint32_t(p.S32(0x3DC)) & 0xFFFFFCFFu);

    p.Down16(0x418);
    p.Down16(0x41E);

    for (const auto [timer, flag] : {std::pair{0x454u, 0x450u}, {0x456u, 0x451u}, {0x458u, 0x452u}}) {
        if (p.Expired16(timer)) {
            p.W16(timer, 0);
            p.W8(flag, 0);
        }
    }

    if (p.U8(2) == 7) {
        if (p.S16(0x468) == 0 && p.S16(0x384) > 0 && p.S32(0x20) % 10 == 0) {
            if (Cheats::On(Cheats::Id::LongSwordPowerMax)) {
                p.W16(0x386, 100);
                p.W16(0x384, 100);
            } else {
                p.W16(0x384, p.U16(0x384) - 1);
            }
        }

        if (uint8_t(game.Call<ArmorLevel>({pl})) > 2 && p.Expired16(0x46A))
            p.W16(0x386, 0);
    }

    if (p.S16(0x45E) > 0) {
        p.W16(0x45E, p.U16(0x45E) - 1);

        if (p.S16(0x45E) == 0 && game.Call<MasterCheck>({pl}) == 1 && game.Call<InEventDemo>({1}) == 0)
            game.Call<ShowStatusMessage>({pl, 1, 31, 0});
    }

    // Hot and cold drink timers: twice as fast on odd steps in their climate.
    for (const auto [timer, climate, message] : {std::tuple{0x464u, 3u, 35u}, {0x466u, 4u, 36u}}) {
        if (p.U16(timer) == 0)
            continue;

        if (uint8_t(game.Call<MapClimate>({p.U8(0x16)})) == climate) {
            p.W16(timer, p.U16(timer) - ((p.S32(0x20) & 1) == 0 ? 1 : 2));

            if (p.S16(timer) < 0)
                p.W16(timer, 0);
        } else {
            p.W16(timer, p.U16(timer) - 1);
        }

        if (p.S16(timer) == 0 && game.Call<MasterCheck>({pl}) == 1 && game.Call<InEventDemo>({1}) == 0)
            game.Call<ShowStatusMessage>({pl, 1, message, 0});
    }

    if (p.U16(0x410) != 0)
        p.W16(0x410, p.U16(0x410) - 1);

    for (const uint32_t timer : {0x42Eu, 0x438u, 0x430u, 0x43Au, 0x432u, 0x43Cu, 0x434u, 0x43Eu, 0x436u, 0x440u, 0x422u})
        p.Down16(timer);

    game.Call<StatusUpdate>({pl});

    if (uint8_t(p.U8(10) - 6) < 3)
        return;

    // Health recovery every N steps (skills change N), not in the wrong climate
    // without the matching skill.
    bool recover = true;

    if (p.S16(0x464) == 0) {
        const uint8_t climate = uint8_t(game.Call<MapClimate>({p.U8(0x16)}));

        if (climate == 1)
            recover = game.Call<SkillCheck>({pl, 124}) != 0 || game.Call<SkillCheck>({pl, 125}) != 0;
        else if (climate == 3)
            recover = game.Call<SkillCheck>({pl, 125}) != 0;
    }

    if (recover && game.Call<CanRecoverHealth>({pl}) == 1) {
        int32_t every = 60;

        if (game.Call<SkillCheck>({pl, 17}) == 1)
            every = 30;

        if (game.Call<SkillCheck>({pl, 18}) == 1)
            every = 15;

        if (game.Call<SkillCheck>({pl, 19}) == 1)
            every = 120;

        if (game.Call<SkillCheck>({pl, 20}) == 1)
            every = 240;

        const int32_t amount = p.S16(0x45E) != 0 ? 2 : 1;

        if (p.S32(0x20) % every == 0 && p.S16(0x370) < p.S16(0x376)) {
            const int32_t missing = int16_t(p.S16(0x376) - p.S16(0x370));
            game.Call<RecoverHealth>({pl, uint32_t(missing < amount ? missing : amount)});
        }
    }

    p.Down16(0x400);

    if (p.S16(0x400) < 1) {
        p.W16(0x400, 0);
        p.Down16(0x3FC);
    }
}

void PlayerStepTimers(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const Plw player{frame.Arg(3)};

    for (uint32_t i = 0, ticks = GameStep::Ticks(); i < ticks; ++i)
        Tick(game, player);
}

} // namespace

extern "C" void func_80260B9C(CpuContext* ctx);

extern "C" void MH3_PlayerStepTimers(CpuContext* ctx) {
    PortCheck::Fuzz(ctx, 'P', ctx->gpr[3]);
    PortCheck::Call(ctx, "PlayerStepTimers", func_80260B9C, PlayerStepTimers);
}
REGISTER_NATIVE_FUNCTION_AS(0x80260B9C, MH3_PlayerStepTimers, "PlayerStepTimers");
