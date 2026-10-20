// The weapons' action functions, RMHP08. Group 4's actions below 100 go to
// the hunter's weapon's function (the action step 0x8025DE9C,
// field_player_actions.cpp), which switches on the action (+12) to the
// action's own function with up to four constant arguments (r4 on), those
// from 100 to the weapons' common one. The tables are the jump tables'
// cases. Ported here (an action past a table does nothing unless noted):
//   0x80282A18  the great sword's (weapon 0): +0x368 = the charge level
//               (+0x312) first; actions 0-65
//   0x80284140  the sword and shield's (weapon 1): actions 0-34
//   0x80286E5C  PlHammerActions (weapon 2): actions 0-56; any other ends the
//               action (FieldEndAction 0, 4). Then, in a charge's motions
//               (1131, 1133, 1155, 1156), the charge level's sound at the
//               hammer's head: 13 at level 1, 14 above it
//   0x8034EE7C  the lance's (weapon 3): actions 0-67
//   0x802888AC  the bowgun's (weapons 4-6): actions 0-49
//   0x8028B394  the long sword's (weapon 7): actions 0-60; any other ends
//   0x803347BC  the switch axe's (weapon 8): actions 0-88; any other ends
//   0x8025B6FC  the weapons' common actions: 100 (0x8025B5A8), 101
//               (0x8025B648); any other ends
//   0x802844D8  (plw) -> u8: the hammer's charge level: the steps charged
//               (+0x310) over the charge time (0x80284454)
//   0x8027C0C8  (plw, VEC3* out): a point on the weapon: an offset (z, by
//               weapon) through one of its joints' matrix, the joint by
//               weapon and 0x80224E8C's state; the lance's offset turns in
//               state 1
#include "player_work.h"
#include "port_check.h"

#include "ppc_runtime.h"

#include <iterator>

namespace {

enum : uint32_t {
    kWeapon = 2,           // u8: the weapon kind (0-8)
    kPlayerNumber = 8,     // u8
    kAction = 12,          // u16: the action within its group
    kChargeSteps = 0x310,  // s16: steps charged
    kChargeLevel = 0x312,  // u8 (0x802812C0)
    kChargeLevel2 = 0x368, // u8: the great sword's copy
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

// An action's function and its arguments (r4 on).
struct Entry {
    uint32_t fn;
    uint8_t count;
    int8_t args[4];
};

constexpr Entry kGreatSword[] = {
    {0x80280410, 1, {0}}, {0x80280520, 1, {0}}, {0x802806DC, 1, {0}}, {0x802806DC, 1, {1}}, {0x80280B4C, 0, {}}, {0x80280C00, 1, {0}}, {0x80280E30, 1, {0}},
    {0x80280F40, 1, {0}}, {0x802806DC, 1, {2}}, {0x802806DC, 1, {3}}, {0x802806DC, 1, {4}}, {0x802806DC, 1, {5}}, {0x80280410, 1, {1}}, {0x80280410, 1, {2}},
    {0x80280410, 1, {3}}, {0x80280410, 1, {4}}, {0x80280520, 1, {1}}, {0x80280520, 1, {2}}, {0x80280520, 1, {3}}, {0x80280520, 1, {4}}, {0x80280C00, 1, {1}},
    {0x80281050, 0, {}}, {0x802806DC, 1, {6}}, {0x80281100, 0, {}}, {0x802813D0, 1, {0}}, {0x802813D0, 1, {1}}, {0x802813D0, 1, {2}}, {0x802813D0, 1, {3}},
    {0x802813D0, 1, {4}}, {0x802813D0, 1, {5}}, {0x802813D0, 1, {6}}, {0x802818B4, 1, {0}}, {0x802818B4, 1, {1}}, {0x802818B4, 1, {2}}, {0x80281C3C, 1, {0}},
    {0x80281DBC, 0, {}}, {0x80281F68, 1, {0}}, {0x80280C00, 1, {2}}, {0x80280C00, 1, {3}}, {0x80280E30, 1, {1}}, {0x80280E30, 1, {2}}, {0x80280F40, 1, {1}},
    {0x80280F40, 1, {2}}, {0x80280C00, 1, {4}}, {0x802821DC, 1, {0}}, {0x802821DC, 1, {1}}, {0x802821DC, 1, {2}}, {0x802821DC, 1, {3}}, {0x80282434, 1, {0}},
    {0x80282530, 0, {}}, {0x80281C3C, 1, {1}}, {0x80281F68, 1, {1}}, {0x80281F68, 1, {2}}, {0x80280520, 1, {5}}, {0x80280520, 1, {6}}, {0x80280410, 1, {5}},
    {0x80280410, 1, {6}}, {0x80280C00, 1, {5}}, {0x802825F0, 1, {0}}, {0x802825F0, 1, {1}}, {0x80280F40, 1, {3}}, {0x80280F40, 1, {3}}, {0x80280E30, 1, {3}},
    {0x80280E30, 1, {3}}, {0x80282770, 0, {}}, {0x8028291C, 1, {0}},
};

constexpr Entry kSwordShield[] = {
    {0x80282D30, 1, {0}}, {0x80282D30, 1, {1}}, {0x80282D30, 1, {2}}, {0x80282D30, 1, {3}}, {0x80282F3C, 1, {0}}, {0x80282F3C, 1, {1}}, {0x80283074, 1, {0}},
    {0x80283074, 1, {1}}, {0x80283074, 1, {2}}, {0x80283250, 1, {2}}, {0x80283250, 1, {0}}, {0x80283250, 1, {1}}, {0x80283458, 1, {0}}, {0x80283458, 1, {1}},
    {0x80283570, 0, {}}, {0x8028363C, 0, {}}, {0x80283708, 1, {0}}, {0x80283708, 1, {1}}, {0x80283708, 1, {2}}, {0x80283874, 1, {1}}, {0x80283A0C, 1, {0}},
    {0x80283A0C, 1, {1}}, {0x80283BEC, 0, {}}, {0x80283CC4, 1, {0}}, {0x80283E14, 0, {}}, {0x80283EE4, 0, {}}, {0x80283F88, 0, {}}, {0x80283250, 1, {3}},
    {0x80283250, 1, {4}}, {0x8028402C, 1, {0}}, {0x8028402C, 1, {1}}, {0x8028402C, 1, {2}}, {0x80283A0C, 1, {2}}, {0x80283CC4, 1, {1}}, {0x80283874, 1, {0}},
};

constexpr Entry kHammer[] = {
    {0x802847E4, 1, {0}}, {0x802848E8, 1, {0}}, {0x802848E8, 1, {1}}, {0x80284A54, 0, {}}, {0x80284B68, 1, {0}}, {0x80284CCC, 0, {}}, {0x80284D70, 0, {}},
    {0x80284E88, 1, {0}}, {0x80284E88, 1, {1}}, {0x80284E88, 1, {2}}, {0x80284E88, 1, {3}}, {0x80285144, 1, {0}}, {0x80285144, 1, {1}}, {0x80285144, 1, {3}},
    {0x80285144, 1, {2}}, {0x80285144, 1, {4}}, {0x80285438, 0, {}}, {0x80285538, 0, {}}, {0x8028563C, 0, {}}, {0x80284B68, 1, {1}}, {0x802858BC, 0, {}},
    {0x80285988, 0, {}}, {0x80285A54, 1, {0}}, {0x80285B58, 1, {0}}, {0x80285B58, 1, {1}}, {0x80285CC8, 0, {}}, {0x80285DD0, 1, {0}}, {0x80285F34, 0, {}},
    {0x80285FD8, 0, {}}, {0x802860E8, 1, {0}}, {0x80286368, 0, {}}, {0x8028648C, 1, {0}}, {0x8028648C, 1, {1}}, {0x802866A0, 1, {0}}, {0x802866A0, 1, {1}},
    {0x80286820, 1, {0}}, {0x802869D4, 0, {}}, {0x80286C54, 1, {0}}, {0x80284E88, 1, {4}}, {0x80284E88, 1, {5}}, {0x802860E8, 1, {1}}, {0x802860E8, 1, {2}},
    {0x802860E8, 1, {3}}, {0x802860E8, 1, {4}}, {0x802847E4, 1, {1}}, {0x802847E4, 1, {2}}, {0x802847E4, 1, {3}}, {0x80285A54, 1, {1}}, {0x80285A54, 1, {2}},
    {0x80285A54, 1, {3}}, {0x8028648C, 1, {2}}, {0x80286D58, 1, {0}}, {0x80285DD0, 1, {1}}, {0x802848E8, 1, {2}}, {0x80285B58, 1, {2}}, {0x80284E88, 1, {6}},
    {0x802860E8, 1, {5}},
};

constexpr Entry kLance[] = {
    {0x8034D580, 2, {0, 0}}, {0x8034D580, 2, {2, 0}}, {0x8034D580, 2, {3, 0}}, {0x8034D980, 2, {0, 0}}, {0x8034D980, 2, {1, 0}}, {0x8034DD8C, 2, {0, 0}},
    {0x8034DEF8, 2, {0, 0}}, {0x8034E0C4, 2, {0, 0}}, {0x8034E0C4, 2, {1, 0}}, {0x8034E0C4, 2, {2, 0}}, {0x8034E234, 1, {0}}, {0x8034D580, 2, {1, 0}},
    {0x8034E320, 2, {0, 0}}, {0x8034E320, 2, {1, 0}}, {0x8034DD8C, 2, {1, 0}}, {0x8034E4B8, 1, {0}}, {0x8034D580, 2, {4, 0}}, {0x8034D580, 2, {0, 1}},
    {0x8034D580, 2, {1, 1}}, {0x8034D580, 2, {2, 1}}, {0x8034D580, 2, {3, 1}}, {0x8034D580, 2, {4, 1}}, {0x8034E0C4, 2, {0, 1}}, {0x8034E0C4, 2, {1, 1}},
    {0x8034E0C4, 2, {2, 1}}, {0x8034E234, 1, {1}}, {0x8034E4B8, 1, {1}}, {0x8034D980, 2, {0, 1}}, {0x8034D980, 2, {1, 1}}, {0x8034DD8C, 2, {0, 1}},
    {0x8034DD8C, 2, {1, 1}}, {0x8034DEF8, 2, {0, 1}}, {0x8034E320, 2, {0, 1}}, {0x8034E320, 2, {1, 1}}, {0x8034E5D8, 1, {0}}, {0x8034E728, 1, {0}},
    {0x8034E0C4, 2, {3, 0}}, {0x8034E0C4, 2, {4, 0}}, {0x8034D580, 2, {5, 0}}, {0x8034E800, 1, {0}}, {0x8034E954, 0, {}}, {0x8034D580, 2, {6, 0}},
    {0x8034EA20, 2, {0, 0}}, {0x8034EA20, 2, {0, 1}}, {0x8034EC54, 1, {0}}, {0x8034EDA4, 1, {0}}, {0x8034E0C4, 2, {3, 1}}, {0x8034E0C4, 2, {4, 1}},
    {0x8034D580, 2, {5, 1}}, {0x8034E800, 1, {1}}, {0x8034EB88, 0, {}}, {0x8034D580, 2, {6, 1}}, {0x8034D580, 2, {7, 1}}, {0x8034D580, 2, {8, 0}},
    {0x8034D580, 2, {8, 1}}, {0x8034E0C4, 2, {5, 0}}, {0x8034E0C4, 2, {5, 1}}, {0x8034DEF8, 2, {1, 1}}, {0x8034EA20, 2, {1, 0}}, {0x8034EA20, 2, {2, 0}},
    {0x8034EA20, 2, {3, 0}}, {0x8034EA20, 2, {1, 1}}, {0x8034EA20, 2, {2, 1}}, {0x8034EA20, 2, {3, 1}}, {0x8034D580, 2, {9, 0}}, {0x8034D580, 2, {9, 1}},
    {0x8034E0C4, 2, {6, 0}}, {0x8034E0C4, 2, {6, 1}},
};

constexpr Entry kBowgun[] = {
    {0x80287438, 0, {}}, {0x80287544, 0, {}}, {0x80287668, 1, {0}}, {0x80287668, 1, {1}}, {0x802878B0, 0, {}}, {0x80287A30, 4, {0, 0, 0, 0}},
    {0x80287A30, 4, {1, 0, 0, 0}}, {0x80287A30, 4, {2, 0, 0, 0}}, {0x80287A30, 4, {0, 1, 0, 0}}, {0x80287A30, 4, {1, 1, 0, 0}}, {0x80287A30, 4, {2, 1, 0, 0}},
    {0x80287A30, 4, {0, 0, 1, 0}}, {0x80287A30, 4, {1, 0, 1, 0}}, {0x80287A30, 4, {2, 0, 1, 0}}, {0x802881AC, 0, {}}, {0x802882D8, 0, {}}, {0x80288380, 1, {0}},
    {0x80288380, 1, {1}}, {0x802885C0, 0, {}}, {0x802886AC, 2, {0, 0}}, {0x80287A30, 4, {3, 0, 0, 0}}, {0x802886AC, 2, {0, 1}}, {0x80287A30, 4, {3, 0, 1, 0}},
    {0x80287A30, 4, {0, 1, 1, 0}}, {0x80287A30, 4, {1, 1, 1, 0}}, {0x80287A30, 4, {2, 1, 1, 0}}, {0x80287A30, 4, {0, 0, 0, 1}}, {0x80287A30, 4, {0, 0, 0, 2}},
    {0x80287A30, 4, {0, 0, 0, 3}}, {0x80287A30, 4, {0, 0, 0, 4}}, {0x80287A30, 4, {0, 1, 0, 1}}, {0x80287A30, 4, {0, 1, 0, 2}}, {0x80287A30, 4, {0, 1, 0, 3}},
    {0x80287A30, 4, {0, 1, 0, 4}}, {0x80287A30, 4, {0, 0, 1, 1}}, {0x80287A30, 4, {0, 0, 1, 2}}, {0x80287A30, 4, {0, 0, 1, 3}}, {0x80287A30, 4, {0, 0, 1, 4}},
    {0x80287A30, 4, {0, 1, 1, 1}}, {0x80287A30, 4, {0, 1, 1, 2}}, {0x80287A30, 4, {0, 1, 1, 3}}, {0x80287A30, 4, {0, 1, 1, 4}}, {0x802886AC, 2, {1, 0}},
    {0x80287A30, 4, {4, 0, 0, 0}}, {0x802886AC, 2, {1, 1}}, {0x80287A30, 4, {4, 0, 1, 0}}, {0x802886AC, 2, {2, 0}}, {0x80287A30, 4, {5, 0, 0, 0}},
    {0x802886AC, 2, {2, 1}}, {0x80287A30, 4, {5, 0, 1, 0}},
};

constexpr Entry kLongSword[] = {
    {0x80288ECC, 2, {0, 0}}, {0x80288ECC, 2, {0, 1}}, {0x80288ECC, 2, {1, 0}}, {0x80288ECC, 2, {2, 0}}, {0x80289190, 0, {}}, {0x80289280, 1, {0}}, {0x80289280, 1, {1}},
    {0x80289438, 0, {}}, {0x8028957C, 1, {0}}, {0x8028957C, 1, {1}}, {0x8028957C, 1, {2}}, {0x8028957C, 1, {3}}, {0x8028957C, 1, {4}}, {0x8028957C, 1, {5}},
    {0x8028957C, 1, {6}}, {0x802896B8, 0, {}}, {0x8028975C, 1, {0}}, {0x8028975C, 1, {1}}, {0x8028975C, 1, {2}}, {0x8028975C, 1, {3}}, {0x8028975C, 1, {4}},
    {0x8028975C, 1, {5}}, {0x80289990, 0, {}}, {0x80289AC4, 1, {0}}, {0x80289AC4, 1, {1}}, {0x80289C3C, 0, {}}, {0x80289D38, 1, {0}}, {0x80289D38, 1, {1}},
    {0x8028A054, 1, {0}}, {0x8028A17C, 0, {}}, {0x8028A24C, 1, {0}}, {0x8028A35C, 0, {}}, {0x8028A054, 1, {1}}, {0x8028A464, 1, {0}}, {0x8028A464, 1, {1}},
    {0x8028A464, 1, {2}}, {0x8028A464, 1, {3}}, {0x8028A464, 1, {4}}, {0x8028A464, 1, {5}}, {0x8028A690, 0, {}}, {0x8028A7C4, 1, {0}}, {0x8028A7C4, 1, {1}},
    {0x8028A93C, 0, {}}, {0x8028AA38, 1, {0}}, {0x8028AA38, 1, {1}}, {0x8028AC78, 1, {0}}, {0x8028AC78, 1, {1}}, {0x8028AE1C, 0, {}}, {0x8028AF1C, 0, {}},
    {0x8028A24C, 1, {1}}, {0x8028A24C, 1, {2}}, {0x8028A24C, 1, {3}}, {0x8028A24C, 1, {4}}, {0x8028A24C, 1, {5}}, {0x8028A054, 1, {2}}, {0x8028AFB4, 1, {0}},
    {0x8028AFB4, 1, {1}}, {0x8028B158, 0, {}}, {0x8028B1FC, 0, {}}, {0x8028B2FC, 0, {}}, {0x8028A054, 1, {3}},
};

constexpr Entry kSwitchAxe[] = {
    {0x803318CC, 1, {0}}, {0x803318CC, 1, {1}}, {0x80331A28, 1, {0}}, {0x80331B48, 1, {0}}, {0x80331C58, 1, {0}}, {0x80331E58, 1, {0}}, {0x80331FB0, 1, {0}},
    {0x803320D4, 1, {0}}, {0x803321FC, 1, {0}}, {0x80332488, 1, {0}}, {0x80332870, 1, {0}}, {0x80332A40, 1, {0}}, {0x80332CC4, 1, {0}}, {0x8027621C, 3, {0, 4, 0}},
    {0x8027621C, 3, {0, 4, 0}}, {0x80331E58, 1, {1}}, {0x803320D4, 1, {1}}, {0x80332DA8, 1, {0}}, {0x80332DA8, 1, {1}}, {0x80331A28, 1, {1}}, {0x80331E58, 1, {2}},
    {0x80332E8C, 1, {0}}, {0x80332488, 1, {1}}, {0x803320D4, 1, {2}}, {0x80332684, 0, {}}, {0x80332FB0, 0, {}}, {0x803320D4, 1, {3}}, {0x803320D4, 1, {4}},
    {0x80331A28, 1, {2}}, {0x80331A28, 1, {3}}, {0x80331FB0, 1, {1}}, {0x80332E8C, 1, {1}}, {0x80332E8C, 1, {2}}, {0x80331E58, 1, {3}}, {0x80331E58, 1, {4}},
    {0x80331E58, 1, {5}}, {0x80333054, 1, {0}}, {0x80333174, 1, {0}}, {0x80333294, 1, {0}}, {0x803333A4, 1, {0}}, {0x803334B4, 1, {0}}, {0x80333740, 1, {0}},
    {0x803339C4, 1, {0}}, {0x80331B48, 1, {1}}, {0x803318CC, 1, {2}}, {0x803318CC, 1, {3}}, {0x80333B84, 1, {0}}, {0x80333CF0, 1, {0}}, {0x80333E14, 1, {0}},
    {0x80333F38, 1, {0}}, {0x80333F38, 1, {1}}, {0x80333054, 1, {1}}, {0x80333B84, 1, {2}}, {0x8033401C, 1, {0}}, {0x8033401C, 1, {1}}, {0x80334214, 0, {}},
    {0x803343FC, 1, {0}}, {0x80333CF0, 1, {1}}, {0x80333E14, 1, {1}}, {0x80333E14, 1, {2}}, {0x80333B84, 1, {1}}, {0x80333B84, 1, {3}}, {0x80333B84, 1, {4}},
    {0x80333B84, 1, {5}}, {0x80333174, 1, {1}}, {0x80333174, 1, {2}}, {0x80333174, 1, {3}}, {0x80333174, 1, {4}}, {0x80333054, 1, {2}}, {0x80333054, 1, {3}},
    {0x803345CC, 1, {0}}, {0x803345CC, 1, {1}}, {0x80332488, 1, {2}}, {0x80332E8C, 1, {3}}, {0x80333E14, 1, {3}}, {0x80334718, 0, {}}, {0x80331E58, 1, {6}},
    {0x80331E58, 1, {7}}, {0x80333B84, 1, {6}}, {0x80333B84, 1, {7}}, {0x80331E58, 1, {8}}, {0x80333B84, 1, {8}}, {0x80331FB0, 1, {2}}, {0x80332870, 1, {1}},
    {0x803343FC, 1, {1}}, {0x80333CF0, 1, {2}}, {0x80332488, 1, {3}}, {0x8033401C, 1, {2}}, {0x8033401C, 1, {3}},
};

template <uint32_t N>
using Memos = const RawDispatchRecord* [N];

// Runs the action's entry; false past the table.
template <uint32_t N>
bool RunAction(GuestCall& game, const Entry (&table)[N], Memos<N>& memos, uint32_t plw) {
    const uint32_t action = Memory::Read16(plw + kAction);

    if (action >= N)
        return false;

    const Entry& e = table[action];
    const auto arg = [&](uint32_t i) { return uint32_t(int32_t(e.args[i])); };

    switch (e.count) {
    case 0:
        game.CallAt(memos[action], e.fn, {plw});
        break;
    case 1:
        game.CallAt(memos[action], e.fn, {plw, arg(0)});
        break;
    case 2:
        game.CallAt(memos[action], e.fn, {plw, arg(0), arg(1)});
        break;
    default:
        game.CallAt(memos[action], e.fn, {plw, arg(0), arg(1), arg(2), arg(3)});
        break;
    }

    return true;
}

// A weapon whose dispatch only switches: `ends` when an action past the
// table ends the action.
template <const auto& kTable, bool ends>
void Actions(CpuContext* ctx) {
    static Memos<std::size(kTable)> memos = {};
    GuestFrame frame(ctx, 16);
    GuestCall game{ctx};

    if (!RunAction(game, kTable, memos, frame.Arg(3)) && ends)
        game.Call<EndAction>({frame.Arg(3), 0, 4, 0});
}

// 0x80282A18.
void GreatSwordActions(CpuContext* ctx) {
    Memory::Write8(ctx->gpr[3] + kChargeLevel2, Memory::Read8(ctx->gpr[3] + kChargeLevel));
    Actions<kGreatSword, false>(ctx);
}

// 0x8025B6FC.
void CommonActions(CpuContext* ctx) {
    GuestFrame frame(ctx, 16);
    GuestCall game{ctx};
    const uint32_t plw = frame.Arg(3);
    const int32_t action = int16_t(Memory::Read16(plw + kAction));

    if (action == 100)
        game.Call<0x8025B5A8>({plw});
    else if (action == 101)
        game.Call<0x8025B648>({plw});
    else
        game.Call<EndAction>({plw, 0, 4, 0});
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
    static Memos<std::size(kHammer)> memos = {};
    GuestFrame frame(ctx, 32);
    GuestCall game{ctx};
    const Plw::Player p{game, frame.Arg(3)};
    const uint32_t plw = p.plw;

    if (!RunAction(game, kHammer, memos, plw))
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
    PortCheck::Call(ctx, "PlSwordActions", func_80284140, Actions<kSwordShield, false>);
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

extern "C" void func_80282A18(CpuContext* ctx);
extern "C" void MH3_PlGreatSwordActions(CpuContext* ctx) {
    PortCheck::Call(ctx, "PlGreatSwordActions", func_80282A18, GreatSwordActions);
}
REGISTER_NATIVE_FUNCTION_AS(0x80282A18, MH3_PlGreatSwordActions, "PlGreatSwordActions");

extern "C" void func_8034EE7C(CpuContext* ctx);
extern "C" void MH3_PlLanceActions(CpuContext* ctx) {
    PortCheck::Call(ctx, "PlLanceActions", func_8034EE7C, Actions<kLance, false>);
}
REGISTER_NATIVE_FUNCTION_AS(0x8034EE7C, MH3_PlLanceActions, "PlLanceActions");

extern "C" void func_802888AC(CpuContext* ctx);
extern "C" void MH3_PlBowgunActions(CpuContext* ctx) {
    PortCheck::Call(ctx, "PlBowgunActions", func_802888AC, Actions<kBowgun, false>);
}
REGISTER_NATIVE_FUNCTION_AS(0x802888AC, MH3_PlBowgunActions, "PlBowgunActions");

extern "C" void func_8028B394(CpuContext* ctx);
extern "C" void MH3_PlLongSwordActions(CpuContext* ctx) {
    PortCheck::Call(ctx, "PlLongSwordActions", func_8028B394, Actions<kLongSword, true>);
}
REGISTER_NATIVE_FUNCTION_AS(0x8028B394, MH3_PlLongSwordActions, "PlLongSwordActions");

extern "C" void func_803347BC(CpuContext* ctx);
extern "C" void MH3_PlSwitchAxeActions(CpuContext* ctx) {
    PortCheck::Call(ctx, "PlSwitchAxeActions", func_803347BC, Actions<kSwitchAxe, true>);
}
REGISTER_NATIVE_FUNCTION_AS(0x803347BC, MH3_PlSwitchAxeActions, "PlSwitchAxeActions");

extern "C" void func_8025B6FC(CpuContext* ctx);
extern "C" void MH3_PlWeaponCommonActions(CpuContext* ctx) {
    PortCheck::Call(ctx, "PlWeaponCommonActions", func_8025B6FC, CommonActions);
}
REGISTER_NATIVE_FUNCTION_AS(0x8025B6FC, MH3_PlWeaponCommonActions, "PlWeaponCommonActions");
