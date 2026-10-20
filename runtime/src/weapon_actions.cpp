// The weapons' action functions, RMHP08. Group 4's actions below 100 go to
// the hunter's weapon's function (the action step 0x8025DE9C,
// field_player_actions.cpp), which switches on the action (+12) to the
// action's own function, most with an argument in r4. Ported here:
//   0x80284140  the sword and shield's (weapon 1): actions 0-34; any other
//               does nothing
//   0x80286E5C  PlHammerActions (weapon 2): actions 0-56; any other ends the
//               action (FieldEndAction 0, 4). Then, in a charge's motions
//               (1131, 1133, 1155, 1156), the charge level's sound at the
//               hammer's head: 13 at level 1, 14 above it
//   0x802844D8  (plw) -> u8: the hammer's charge level: the steps charged
//               (+0x310) over the charge time (0x80284454)
//   0x8027C0C8  (plw, VEC3* out): a point on the weapon: an offset (z, by
//               weapon) through one of its joints' matrix, the joint by
//               weapon and 0x80224E8C's state; the lance's offset turns in
//               state 1
#include "player_work.h"
#include "port_check.h"

#include "ppc_runtime.h"

namespace {

enum : uint32_t {
    kWeapon = 2,           // u8: the weapon kind (0-8)
    kPlayerNumber = 8,     // u8
    kAction = 12,          // u16: the action within its group
    kChargeSteps = 0x310,  // s16: steps charged
    kSounds = 0xAFC,       // se_w*
};

enum : uint32_t {
    EndAction = 0x8027621C,     // FieldEndAction (plw, u8 flag9, s32 blend, s32 flags)
    HammerChargeTime = 0x80284454, // (plw) -> s16 steps (cheat_hunter.cpp)
    WeaponPoint = 0x8027C0C8,   // (plw, VEC3* out), below
    ShellSound = 0x800DE360,    // shell_se_req (se_w*, VEC3*, u8 id, u8 player)
    WeaponState = 0x80224E8C,   // (plw, u8) -> u8
    JointMatrix = 0x8026A3F8,   // (plw, joint, MTX34* out)
    MtxIdentity = 0x80050538,   // (MTX34*)
    MtxRotDegrees = 0x8008C4E8, // (MTX34*; f1-f3 degrees)
    MtxConcat = 0x800515D8,     // (MTX34* a, MTX34* b)
    MulVecMtx = 0x800514A4,     // (VEC3* inout, MTX34*)
};

constexpr uint32_t kWeaponStateKinds = 0x805BB250; // u8 per weapon: 0x80224E8C's argument
constexpr uint32_t kJoints = 0x805C6928;           // u32 [weapon][3]: by state 0, 1, 2 and above
constexpr uint32_t kLengths = 0x805C6994;          // f32 per weapon: the offset in states 0 and 1
constexpr uint32_t kLanceAngles = 0x805BB4AC;      // VEC3 degrees: the lance's offset turn in state 1

// An action's function and its argument (r4), -1 for none.
struct Entry {
    uint32_t fn;
    int32_t arg;
};

constexpr Entry kSwordActions[] = {
    {0x80282D30, 0}, {0x80282D30, 1}, {0x80282D30, 2}, {0x80282D30, 3}, {0x80282F3C, 0}, {0x80282F3C, 1}, {0x80283074, 0},
    {0x80283074, 1}, {0x80283074, 2}, {0x80283250, 2}, {0x80283250, 0}, {0x80283250, 1}, {0x80283458, 0}, {0x80283458, 1},
    {0x80283570, -1}, {0x8028363C, -1}, {0x80283708, 0}, {0x80283708, 1}, {0x80283708, 2}, {0x80283874, 1}, {0x80283A0C, 0},
    {0x80283A0C, 1}, {0x80283BEC, -1}, {0x80283CC4, 0}, {0x80283E14, -1}, {0x80283EE4, -1}, {0x80283F88, -1}, {0x80283250, 3},
    {0x80283250, 4}, {0x8028402C, 0}, {0x8028402C, 1}, {0x8028402C, 2}, {0x80283A0C, 2}, {0x80283CC4, 1}, {0x80283874, 0},
};

constexpr Entry kHammerActions[] = {
    {0x802847E4, 0}, {0x802848E8, 0}, {0x802848E8, 1}, {0x80284A54, -1}, {0x80284B68, 0}, {0x80284CCC, -1}, {0x80284D70, -1},
    {0x80284E88, 0}, {0x80284E88, 1}, {0x80284E88, 2}, {0x80284E88, 3}, {0x80285144, 0}, {0x80285144, 1}, {0x80285144, 3},
    {0x80285144, 2}, {0x80285144, 4}, {0x80285438, -1}, {0x80285538, -1}, {0x8028563C, -1}, {0x80284B68, 1}, {0x802858BC, -1},
    {0x80285988, -1}, {0x80285A54, 0}, {0x80285B58, 0}, {0x80285B58, 1}, {0x80285CC8, -1}, {0x80285DD0, 0}, {0x80285F34, -1},
    {0x80285FD8, -1}, {0x802860E8, 0}, {0x80286368, -1}, {0x8028648C, 0}, {0x8028648C, 1}, {0x802866A0, 0}, {0x802866A0, 1},
    {0x80286820, 0}, {0x802869D4, -1}, {0x80286C54, 0}, {0x80284E88, 4}, {0x80284E88, 5}, {0x802860E8, 1}, {0x802860E8, 2},
    {0x802860E8, 3}, {0x802860E8, 4}, {0x802847E4, 1}, {0x802847E4, 2}, {0x802847E4, 3}, {0x80285A54, 1}, {0x80285A54, 2},
    {0x80285A54, 3}, {0x8028648C, 2}, {0x80286D58, 0}, {0x80285DD0, 1}, {0x802848E8, 2}, {0x80285B58, 2}, {0x80284E88, 6},
    {0x802860E8, 5},
};

constexpr uint32_t kSwordCount = sizeof(kSwordActions) / sizeof(Entry), kHammerCount = sizeof(kHammerActions) / sizeof(Entry);

static_assert(kSwordCount == 35 && kHammerCount == 57);

// Runs the action's entry; false past the table.
template <uint32_t N>
bool RunAction(GuestCall& game, const Entry (&table)[N], const RawDispatchRecord* (&memos)[N], uint32_t plw) {
    const uint32_t action = Memory::Read16(plw + kAction);

    if (action >= N)
        return false;

    const Entry& entry = table[action];

    if (entry.arg < 0)
        game.CallAt(memos[action], entry.fn, {plw});
    else
        game.CallAt(memos[action], entry.fn, {plw, uint32_t(entry.arg)});

    return true;
}

// 0x80284140.
void SwordActions(CpuContext* ctx) {
    static const RawDispatchRecord* memos[kSwordCount] = {};
    GuestFrame frame(ctx, 16);
    GuestCall game{ctx};
    RunAction(game, kSwordActions, memos, frame.Arg(3));
}

// 0x802844D8.
uint32_t HammerChargeLevel(GuestCall& game, uint32_t plw) {
    const int32_t time = int16_t(game.Call<HammerChargeTime>({plw}));
    return uint32_t(PPC_Divw(int16_t(Memory::Read16(plw + kChargeSteps)), time)) & 0xFF;
}

void HammerChargeLevelPort(CpuContext* ctx) {
    uint32_t level;

    {
        GuestFrame frame(ctx, 16);
        GuestCall game{ctx};
        level = HammerChargeLevel(game, frame.Arg(3));
    }

    ctx->gpr[3] = level;
}

// 0x80286E5C.
void HammerActions(CpuContext* ctx) {
    static const RawDispatchRecord* memos[kHammerCount] = {};
    GuestFrame frame(ctx, 32);
    GuestCall game{ctx};
    const Plw::Player p{game, frame.Arg(3)};
    const uint32_t plw = p.plw;

    if (!RunAction(game, kHammerActions, memos, plw))
        game.Call<EndAction>({plw, 0, 4, 0});

    const uint32_t motion = p.Motion();

    if (motion != 1155 && motion != 1156 && motion != 1131 && motion != 1133)
        return;

    const uint32_t level = HammerChargeLevel(game, plw);

    if (level == 0)
        return;

    const uint32_t at = frame[8];
    game.Call<WeaponPoint>({plw, at});
    game.Call<ShellSound>({Memory::Read32(plw + kSounds), at, level == 1 ? 13u : 14u, Memory::Read8(plw + kPlayerNumber)});
}

// 0x8027C0C8.
void WeaponPointPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 144);
    GuestCall game{ctx};
    const uint32_t plw = frame.Arg(3), out = frame.Arg(4);
    const uint32_t angles = frame[20], turn = frame[32], joint = frame[80];
    const uint32_t weapon = Memory::Read8(plw + kWeapon);
    const uint32_t state = game.Call<WeaponState>({plw, Memory::Read8(kWeaponStateKinds + weapon)}) & 0xFF;
    const uint32_t zero = Memory::Read32(frame.Arg(2) - 14876);
    Memory::Write32(out, zero);
    Memory::Write32(out + 4, zero);

    for (uint32_t k = 0; k < 12; k += 4)
        Memory::Write32(angles + k, zero);

    uint32_t node;

    if (state > 1) {
        node = Memory::Read32(kJoints + (weapon * 3 + 2) * 4);
        Memory::Write32(out + 8, zero);
    } else {
        if (state == 1 && weapon == 3)
            for (uint32_t k = 0; k < 12; k += 4)
                Memory::Write32(angles + k, Memory::Read32(kLanceAngles + k));

        node = Memory::Read32(kJoints + (weapon * 3 + state) * 4);
        Memory::Write32(out + 8, Memory::Read32(kLengths + weapon * 4));
    }

    game.Call<JointMatrix>({plw, node, joint});
    game.Call<MtxIdentity>({turn});
    game.Call<MtxRotDegrees>({turn}, {Memory::ReadFloat32(angles), Memory::ReadFloat32(angles + 4), Memory::ReadFloat32(angles + 8)});
    game.Call<MtxConcat>({joint, turn});
    game.Call<MulVecMtx>({out, joint});

    for (uint32_t k = 0; k < 3; ++k)
        Memory::WriteFloat32(out + 4 * k, Memory::ReadFloat32(out + 4 * k) + Memory::ReadFloat32(joint + 16 * k + 12));
}

} // namespace

extern "C" void func_80284140(CpuContext* ctx);
extern "C" void MH3_PlSwordActions(CpuContext* ctx) {
    PortCheck::Call(ctx, "PlSwordActions", func_80284140, SwordActions);
}
REGISTER_NATIVE_FUNCTION_AS(0x80284140, MH3_PlSwordActions, "PlSwordActions");

extern "C" void func_80286E5C(CpuContext* ctx);
extern "C" void MH3_PlHammerActions(CpuContext* ctx) {
    PortCheck::Call(ctx, "PlHammerActions", func_80286E5C, HammerActions);
}
REGISTER_NATIVE_FUNCTION_AS(0x80286E5C, MH3_PlHammerActions, "PlHammerActions");

extern "C" void func_802844D8(CpuContext* ctx);
extern "C" void MH3_PlHammerChargeLevel(CpuContext* ctx) {
    PortCheck::Call(ctx, "PlHammerChargeLevel", func_802844D8, HammerChargeLevelPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x802844D8, MH3_PlHammerChargeLevel, "PlHammerChargeLevel");

extern "C" void func_8027C0C8(CpuContext* ctx);
extern "C" void MH3_PlWeaponPoint(CpuContext* ctx) {
    PortCheck::Call(ctx, "PlWeaponPoint", func_8027C0C8, WeaponPointPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8027C0C8, MH3_PlWeaponPoint, "PlWeaponPoint");
