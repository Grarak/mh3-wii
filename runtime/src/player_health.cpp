// Player health, RMHP08. C port of PlAddHealth (0x802762F0), the one place
// the hunter's health changes: damage, potions and regeneration all add a
// signed amount through it.
//
// Player work (PLW): +0x0A u8 action group (8: down), +0x370 s16 health,
// +0x372 s16 maximum health, +0x376 s16 recoverable ("red") health.
//
// The optional --no-damage cheat drops the negative amounts (Doudley's
// Invincible players took the event scene's test for damage, the same). The
// Gecko codes patched here (cheats.h), by the instruction they replaced:
//   - Health never changes (Seth's Toggle infinite health, switched by its
//     buttons, and Bully's Infinite health, always): the add;
//   - Alternate infinite health (Seth's): the health's store, so only the
//     clamps below write (empty at 0 or less, full at the maximum);
//   - Daredevil mode: the branch over the clamp at 0, so any change empties
//     the health.
#include "player_health.h"

#include "abi_bridge.h"
#include "cheats.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"

namespace {

enum : uint32_t {
    kActionGroup = 0x0A,
    kHealth = 0x370,
    kMaxHealth = 0x372,
    kRedHealth = 0x376
};

constexpr uint8_t kActionDown = 8;
enum : uint32_t {
    Pl_master_ck = 0x8026FE38,  // (PLW*) -> non-zero for the player this console controls
    PlInEventDemo = 0x8027BCAC, // (0) -> 1 during an event scene
};

bool noDamage = false;

int16_t Read16s(uint32_t address) {
    return int16_t(Memory::Read16(address));
}

} // namespace

void PlayerHealth::SetNoDamage(bool enabled) {
    noDamage = enabled;
}

// 0x802762F0 PlAddHealth(PLW* player, s16 amount).
extern "C" void func_802762F0(CpuContext* ctx);
static void PlAddHealthPort(CpuContext* ctx) {
    const uint32_t player = ctx->gpr[3];
    const auto amount = int16_t(ctx->gpr[4]);
    {
        GuestFrame frame{ctx, 16};
        GuestCall game{ctx};

        if (!game.Call<Pl_master_ck>({player}))
            return;

        if (game.Call<PlInEventDemo>({0}) == 1 && amount < 0)
            return;
    }

    if (Memory::Read8(player + kActionGroup) == kActionDown)
        return;

    if (noDamage && amount < 0)
        return;

    using Cheats::Id;

    const int16_t current = Read16s(player + kHealth);
    int16_t health = int16_t(current + amount);

    if (Cheats::Toggled(Id::HealthNeverChanges))
        health = current;

    if (!Cheats::On(Id::AltInfiniteHealth))
        Memory::Write16(player + kHealth, uint16_t(health));

    if (health <= 0 || Cheats::On(Id::Daredevil)) {
        health = 0;
        Memory::Write16(player + kHealth, 0);
    }

    const int16_t maxHealth = Read16s(player + kMaxHealth);

    if (health >= maxHealth) {
        health = maxHealth;
        Memory::Write16(player + kHealth, uint16_t(health));
    }

    if (Read16s(player + kRedHealth) < health)
        Memory::Write16(player + kRedHealth, uint16_t(health));
}

extern "C" void MH3_PlAddHealth(CpuContext* ctx) {
    PortCheck::Call(ctx, "PlAddHealth", func_802762F0, PlAddHealthPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x802762F0, MH3_PlAddHealth, "PlAddHealth");
