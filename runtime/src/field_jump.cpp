// Field jump, RMHP08. --quest leaves the village
// for a quest's field as soon as the village is up, the way its exits do.
//
// Every trip out of the village is a quest: the Moga Woods exit starts the
// free-roam quest of the current season (1..4, 0x802FB8A4), the quest counter
// the accepted quest. The quest's data sets the map, area and monsters. The
// village task (0x8021F40C, a tcb: +8 phase, +9 state, +10 sub-state) leaves
// in state 5: its main state (0x8021F840) first records the quest id in the
// departure block 0x8065B534 (u16 id, +9 a village setting 0x8021F29C, +11 1
// for the offline field) and 0x8065B238, and from sub-state 2 of state 5 the
// village is torn down, play mode 3 and game mode 1 set and the field task
// 0x8028DE30 started.
//
// Quests come from a quest file the field loads (0x803AD264): offline the
// village's (05/quest00, ids 1000..), in play mode 4 (online) Port Tanzia's
// (05/quest01, ids 10000..); the arena's (05/quest03, ids 9000..) through
// 0x803AD744. The jump loads the file of its quest.
//
// The quest's map setup (0x803AF518) puts its start area in the field's move
// work (+0xEA, copied to +0xEE and the current area 0x806BB806); the stage
// then loads that area. --area n replaces it with the area the map numbers n
// (main.cpp converts it: QuestCatalog::AreaIndex), --area-index n with the
// game's area n.
//
// --spawn x,y,z moves the hunter there once the quest has placed them (the
// player's move work, kind 2 in the table 0x8065AB04, position at +0x3C) and
// that placement has held for kSpawnSettle frames: the field places the hunter
// more than once while it starts. The arena's start area is its waiting room;
// --spawn 0,300,0 drops the hunter into the pit.
//
// --spawn exit:n instead puts the hunter, kExitSettle steps into the field,
// into the current area's exit toward area n (numbered as on the map), and the
// game changes area as if they had walked there: the exit check (0x802B2754,
// each step of the field) tests, while the hunter may leave, their position
// against the area's exits (0x802AFEC0(map, area): 0x34-byte records up to an
// s8 -1, +0 the area they lead to, +2 the shape, 0 a cylinder at +4 x, +8 y,
// +0xC z) and on a hit starts the change (0x8028D150(area, the position +0x24
// there, the angle +0x30)).
//
// --spawn exit:tour keeps going: kExitSettle steps after each arrival the
// hunter takes the area's next exit in turn (each area's exits in rotation,
// a rotor walk, which takes every exit of the areas it reaches), printing
// each change, so one run crosses every area change of the map (1.11).
//
// The arena (map 9) runs here as an offline field (play mode 3), not in its
// own play mode 2 (FieldTaskVs 0x8028E58C). The waiting room is part of the
// pit's area, beyond z -10000, and the field work marks per player whether
// they are in the pit (u8 +0x136 + player index, 0x8028EF94): only in play
// mode 2 does the gate's area change (0x8028D150) toggle the mark. A monster
// senses a hunter (0x8012D118, for its sight, targets and attacks) in its
// own area, and on the arena map only on the hunter's side: an unmarked
// hunter is sensed by monsters below z -10000 (0x802B06EC), a marked one by
// the rest. With the mark never set, no monster in the pit notices the
// hunter. Here the mark follows the hunter's side, however they got there:
// --spawn x,y,z, or the waiting room's gate (a cylinder exit to the pit's own
// area, walked through or --spawn exit:<area>).
//
// The village runs 30 steps in its main state before leaving.
#include "field_jump.h"

#include "abi_bridge.h"
#include "guest_call.h"
#include "load_progress.h"
#include "memory.h"
#include "port_check.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

extern "C" void func_8021F40C(CpuContext* ctx);

namespace {

constexpr uint32_t kQuestId = 0x8065B238;   // u32: the departing quest
constexpr uint32_t kDeparture = 0x8065B534; // u16 quest id, +9 u8, +11 u8 offline field
enum : uint32_t {
    VillageSetting = 0x8021F29C, // () -> u8, recorded at departure +9
    FreeRoamQuest = 0x802FB8A4,  // () -> the season's free-roam quest id, 0 if none
    PrimInitAll = 0x800E3DA4,
    FilterReset = 0x80058514,
};

constexpr uint32_t kFirstCounterQuest = 1000; // free-roam quests are 1..4
constexpr uint32_t kPlayMode = 0x8065AA85;    // u8
constexpr uint8_t kPlayModeOnline = 4;
constexpr uint8_t kPlayModeArena = 2;         // FieldTaskVs
constexpr uint32_t MoveWork = 0x800CFAF4;       // get_move_work_adrs(u8 kind) -> move work
constexpr uint32_t kCurrentArea = 0x806BB806;   // u8, get_now_areano
constexpr uint32_t kMoveWorkTable = 0x8065AB04; // +16: move works by kind
constexpr uint32_t kPlayerMoveWork = 2;
enum : uint32_t {
    kPosition = 0x3C
};

constexpr uint64_t kSpawnSettle = 60; // game frames (30 Hz)
enum : uint32_t {
    MyPlayerIndex = 0x800CF3E8,
    GetNowMapNumber = 0x802AFCD8,
    GetNowAreaNumber = 0x802AFCE8,
    AreaExits = 0x802AFEC0, // (map, area): the area's exits
};

constexpr uint32_t kExitSettle = 60; // exit checks (game steps) in the field
constexpr uint32_t kExitBytes = 0x34;
constexpr uint16_t kExitCylinder = 0;
enum : uint32_t {
    kWorkArea = 0xEA,
    kWorkStartArea = 0xEE,
    kWorkState = 0xFA, // 2, 3: in the field
    kWorkInPit = 0x136 // u8 per player (two)
};

constexpr uint32_t kCurrentMap = 0x806BB805; // u8, get_now_mapno
constexpr uint32_t kMyPlayer = 0x8065AA87;   // u8, what MyPlayerIndex returns
constexpr uint32_t kPlayerBytes = 0xB20;
constexpr uint8_t kArenaMap = 9;
constexpr float kArenaWaitingRoomZ = -10000.0f; // below: the waiting room (0x802B06EC)
constexpr uint32_t kQuestWork = 0x80797100;     // u32: +0x3C the quest's data, its +0x8B the quest's map
constexpr uint8_t kWaterArenaMap = 10;          // the quest's map; the field loads map 9, area 1

enum : uint32_t {
    kPhase = 8,
    kState = 9,
    kSubState = 10
};

enum : uint8_t {
    kStateLeave = 5,
    kSubStateTearDown = 2
};

struct Jump {
    bool enabled = false;
    uint32_t quest = 0; // 0: the free-roam quest
    QuestCatalog::Set set = QuestCatalog::Set::Village;
    uint32_t wait = 30;
    int area = -1; // -1: the quest's
    uint32_t steps = 0;
    bool done = false;
    int lastState = -1;
    bool spawn = false;
    float spawnPos[3] = {};
    std::vector<int> exitPath; // --spawn exit:n[,m...], the areas still to walk to
    int exitArea = -1;         // the next of them, -1: none
    uint32_t exitChecks = 0; // exit checks in the field (in the area, touring) so far
    bool tour = false;       // --spawn exit:tour
    int tourArea = -1;       // the area the tour last saw
    uint32_t tourChanges = 0;
    uint8_t tourRotor[64] = {}; // per area: its next exit
    uint64_t placedAt = 0; // game frame the hunter was first seen placed, 0: not yet
    bool loading = false;  // the quest's loading screen seen
    bool entered = false;  // the field entry reported
};

Jump& State() {
    static Jump jump;
    return jump;
}

} // namespace

static void VillageTaskPort(CpuContext* ctx) {
    Jump& jump = State();
    const uint32_t tcb = ctx->gpr[3];

    if (jump.enabled && !jump.done && Memory::Read8(tcb + kPhase) == 1) {
        const int state = Memory::Read8(tcb + kState);

        if (state != jump.lastState) {
            std::fprintf(stderr, "MH3 field jump: village task state %d\n", state);
            jump.lastState = state;
            jump.steps = 0;
        }

        if (state == 2 && ++jump.steps >= jump.wait) {
            // As the village's departure (0x8021FC10), then state 5.
            const CpuContext saved = *ctx;
            GuestCall game{ctx};
            const uint32_t quest = jump.quest ? jump.quest : game.Call<FreeRoamQuest>() & 0xFFFF;
            std::fprintf(stderr, "MH3 field jump: leaving the village for quest %u\n", quest);

            // A free-roam quest leaves for the offline field; a quest from the
            // counter through the quest departure (0x8021F40C, play mode 6).
            Memory::Write8(kDeparture + 11, quest < kFirstCounterQuest ? 1 : 0);
            Memory::Write8(kDeparture + 9, uint8_t(game.Call<VillageSetting>()));
            Memory::Write16(kDeparture, uint16_t(quest));
            Memory::Write32(kQuestId, quest);
            game.Call<PrimInitAll>();
            game.Call<FilterReset>();
            *ctx = saved;
            Memory::Write8(tcb + kState, kStateLeave);
            Memory::Write8(tcb + kSubState, kSubStateTearDown);
            jump.done = true;
        }
    }

    func_8021F40C(ctx);
}

extern "C" void MH3_VillageTask(CpuContext* ctx) {
    PortCheck::Call(ctx, "VillageTask", func_8021F40C, VillageTaskPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8021F40C, MH3_VillageTask, "VillageTask");

void RequestQuestJump(uint32_t quest, QuestCatalog::Set set, int area) {
    Jump& jump = State();
    jump.enabled = true;
    jump.quest = quest;
    jump.set = set;
    jump.area = area;
}

void RequestQuestSpawn(float x, float y, float z) {
    Jump& jump = State();
    jump.spawn = true;
    jump.spawnPos[0] = x;
    jump.spawnPos[1] = y;
    jump.spawnPos[2] = z;
}

void RequestQuestExit(std::vector<int> areas) {
    Jump& jump = State();
    jump.exitPath = std::move(areas);
    jump.exitArea = jump.exitPath.empty() ? -1 : jump.exitPath.front();
}

void RequestQuestTour() {
    State().tour = true;
}

// The arena's mark of the hunter in the pit, from the hunter's side.
static void KeepArenaPit() {
    const uint32_t table = Memory::Read32(kMoveWorkTable);
    const uint32_t work = table ? Memory::Read32(table + 16) : 0;
    const uint32_t players = table ? Memory::Read32(table + 16 + kPlayerMoveWork * 4) : 0;
    const uint32_t index = Memory::Read8(kMyPlayer);

    if (!work || !players || index > 1 || Memory::Read8(kCurrentMap) != kArenaMap || uint8_t(Memory::Read8(work + kWorkState) - 2) > 1)
        return;

    const uint32_t plw = players + index * kPlayerBytes;
    const uint8_t inPit = Memory::ReadFloat32(plw + kPosition + 8) >= kArenaWaitingRoomZ;

    if (!Memory::Read8(plw) || Memory::Read8(work + kWorkInPit + index) == inPit)
        return;

    Memory::Write8(work + kWorkInPit + index, inPit);
    std::fprintf(stderr, "MH3 field jump: the hunter is %s the arena's pit\n", inPit ? "in" : "out of");
}

void FieldJumpFrame(uint64_t gameFrame) {
    Jump& jump = State();

    if (jump.done && jump.set == QuestCatalog::Set::Arena)
        KeepArenaPit();

    if (!jump.done || (!jump.spawn && jump.entered))
        return;

    jump.loading |= LoadProgress::Showing();
    const uint32_t table = Memory::Read32(kMoveWorkTable);
    const uint32_t plw = table ? Memory::Read32(table + 16 + kPlayerMoveWork * 4) : 0;

    if (!plw || !Memory::Read8(plw) || Memory::ReadFloat32(plw + kPosition) == 0.0f) {
        jump.placedAt = 0;
        return;
    }

    // Once, after the quest's loading screen: for scripts that time a run
    // from the field (scripts/pgo/collect-pi.sh).
    if (!jump.entered && jump.loading && !LoadProgress::Showing()) {
        jump.entered = true;
        std::fprintf(stderr, "MH3 field jump: the hunter is in the field\n");
    }

    if (!jump.spawn)
        return;

    if (!jump.placedAt)
        jump.placedAt = gameFrame;

    if (gameFrame - jump.placedAt < kSpawnSettle)
        return;

    for (int i = 0; i < 3; ++i)
        Memory::WriteFloat32(plw + kPosition + 4 * i, jump.spawnPos[i]);

    std::fprintf(stderr, "MH3 field jump: moved the hunter to %.1f %.1f %.1f\n", jump.spawnPos[0], jump.spawnPos[1], jump.spawnPos[2]);
    jump.spawn = false;
}

// The hunter's exit check; first, for --spawn exit:n, the hunter into that
// exit.
extern "C" void func_802B2754(CpuContext* ctx);

// An area for messages: the number its map shows and the game's index.
static std::string AreaText(int area) {
    return QuestCatalog::AreaName(Memory::Read8(kCurrentMap), area);
}

// The tour's next exit: the current area's exits in turn. An exit that left
// the hunter in the area kExitSettle * 4 checks later is passed over.
static void TourStep(CpuContext* ctx, Jump& jump) {
    const CpuContext saved = *ctx;
    GuestCall game{ctx};
    const int8_t area = int8_t(game.Call<GetNowAreaNumber>());

    if (area < 0 || area >= int8_t(sizeof jump.tourRotor)) {
        *ctx = saved;
        return;
    }

    if (area != jump.tourArea) {
        if (jump.tourArea >= 0)
            std::fprintf(stderr, "MH3 field jump: tour change %u: in %s\n", ++jump.tourChanges, AreaText(area).c_str());

        jump.tourArea = area;
        jump.exitChecks = 0;
    }

    if (++jump.exitChecks == kExitSettle * 5)
        jump.exitChecks = kExitSettle;

    if (jump.exitChecks != kExitSettle) {
        *ctx = saved;
        return;
    }

    const uint32_t player = game.Call<MoveWork>({kPlayerMoveWork}) + 0xB20 * uint32_t(int8_t(game.Call<MyPlayerIndex>()));
    const uint32_t exits = game.Call<AreaExits>({uint8_t(game.Call<GetNowMapNumber>()), uint8_t(area)});
    *ctx = saved;
    uint32_t count = 0;

    for (uint32_t e = exits; e && int8_t(Memory::Read8(e)) != -1; e += kExitBytes)
        count += Memory::Read16(e + 2) == kExitCylinder;

    if (!count) {
        std::fprintf(stderr, "MH3 field jump: tour: %s has no exit\n", AreaText(area).c_str());
        return;
    }

    const uint32_t pick = jump.tourRotor[area]++ % count;
    uint32_t exit = exits;

    for (uint32_t seen = 0;; exit += kExitBytes)
        if (Memory::Read16(exit + 2) == kExitCylinder && seen++ == pick)
            break;

    for (int i = 0; i < 3; ++i)
        Memory::WriteFloat32(player + kPosition + 4 * i, Memory::ReadFloat32(exit + 4 + 4 * i));

    std::fprintf(stderr, "MH3 field jump: tour: %s, exit %u of %u, to %s\n", AreaText(area).c_str(), pick + 1, count, AreaText(int8_t(Memory::Read8(exit))).c_str());
}

extern "C" void MH3_PlayerAreaExit(CpuContext* ctx) {
    Jump& jump = State();

    if (jump.tour && jump.done)
        TourStep(ctx, jump);

    // Arrived in the walk's next area: on to the one after it, or the walk
    // is done (the line scripts time a run from, scripts/pgo/collect-pi.sh).
    if (jump.exitArea >= 0 && jump.done && jump.exitChecks >= kExitSettle) {
        const CpuContext saved = *ctx;
        const int8_t area = int8_t(GuestCall{ctx}.Call<GetNowAreaNumber>());
        *ctx = saved;

        if (area == jump.exitArea) {
            jump.exitPath.erase(jump.exitPath.begin());
            jump.exitArea = jump.exitPath.empty() ? -1 : jump.exitPath.front();
            jump.exitChecks = 0;

            if (jump.exitArea < 0)
                std::fprintf(stderr, "MH3 field jump: the hunter walked to %s\n", AreaText(area).c_str());
        }
    }

    if (jump.exitArea >= 0 && jump.done && ++jump.exitChecks == kExitSettle) {
        const CpuContext saved = *ctx;
        GuestCall game{ctx};
        const uint32_t player = game.Call<MoveWork>({kPlayerMoveWork}) + 0xB20 * uint32_t(int8_t(game.Call<MyPlayerIndex>()));
        const int8_t area = int8_t(game.Call<GetNowAreaNumber>());
        uint32_t exit = area < 0 ? 0 : game.Call<AreaExits>({uint8_t(game.Call<GetNowMapNumber>()), uint8_t(area)});
        const uint32_t exits = exit;
        *ctx = saved;

        for (; exit && int8_t(Memory::Read8(exit)) != -1; exit += kExitBytes)
            if (int8_t(Memory::Read8(exit)) == jump.exitArea && Memory::Read16(exit + 2) == kExitCylinder)
                break;

        if (!exit || int8_t(Memory::Read8(exit)) == -1) {
            std::fprintf(stderr, "MH3 field jump: %s has no exit to %s (exits to", AreaText(area).c_str(), AreaText(jump.exitArea).c_str());

            for (uint32_t e = exits; e && int8_t(Memory::Read8(e)) != -1; e += kExitBytes)
                if (Memory::Read16(e + 2) == kExitCylinder)
                    std::fprintf(stderr, " %s;", AreaText(int8_t(Memory::Read8(e))).c_str());

            std::fprintf(stderr, ")\n");
        } else {
            for (int i = 0; i < 3; ++i)
                Memory::WriteFloat32(player + kPosition + 4 * i, Memory::ReadFloat32(exit + 4 + 4 * i));

            std::fprintf(stderr, "MH3 field jump: moved the hunter into the exit of %s to %s\n", AreaText(area).c_str(), AreaText(jump.exitArea).c_str());
        }
    }

    func_802B2754(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x802B2754, MH3_PlayerAreaExit, "PlayerAreaExit");

// Loads the field's quest file: the jump's quest set.
extern "C" void func_803AD264(CpuContext* ctx);
extern "C" void func_803AD744(CpuContext* ctx);
static void LoadQuestFilePort(CpuContext* ctx) {
    const Jump& jump = State();

    if (jump.done && jump.set == QuestCatalog::Set::Arena) {
        func_803AD744(ctx);
        return;
    }

    if (!jump.done || jump.set != QuestCatalog::Set::Online) {
        func_803AD264(ctx);
        return;
    }

    const uint8_t playMode = Memory::Read8(kPlayMode);
    Memory::Write8(kPlayMode, kPlayModeOnline);
    func_803AD264(ctx);
    Memory::Write8(kPlayMode, playMode);
}

extern "C" void MH3_LoadQuestFile(CpuContext* ctx) {
    PortCheck::Call(ctx, "LoadQuestFile", func_803AD264, LoadQuestFilePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803AD264, MH3_LoadQuestFile, "LoadQuestFile");

// The quest's map setup; then the chosen start area. The arena map (9) holds
// both arenas: area 0 the land arena, area 1 the water arena, each with its
// waiting room. Outside the arena's own play mode (2) the setup starts every
// arena quest in area 0, so a water arena quest (map 10) ran in the land
// arena; it starts in area 1.
extern "C" void func_803AF518(CpuContext* ctx);
static void QuestMapSetupPort(CpuContext* ctx) {
    func_803AF518(ctx);
    const Jump& jump = State();
    const uint32_t questWork = Memory::Read32(kQuestWork);
    const uint32_t quest = questWork ? Memory::Read32(questWork + 0x3C) : 0;
    const bool water = quest && Memory::Read8(quest + 0x8B) == kWaterArenaMap && Memory::Read8(kPlayMode) != kPlayModeArena;
    const int area = jump.done && jump.area >= 0 ? jump.area : water ? 1 : -1;

    if (area < 0)
        return;

    const CpuContext saved = *ctx;
    GuestCall game{ctx};
    const uint32_t work = game.Call<MoveWork>({0});
    *ctx = saved;

    if (!work)
        return;

    const uint8_t map = quest ? Memory::Read8(quest + 0x8B) : 0;
    std::fprintf(stderr, "MH3 quest: starting in %s instead of %s\n", QuestCatalog::AreaName(map, area).c_str(), QuestCatalog::AreaName(map, Memory::Read8(work + kWorkArea)).c_str());
    Memory::Write8(work + kWorkArea, uint8_t(area));
    Memory::Write8(work + kWorkStartArea, uint8_t(area));
    Memory::Write8(kCurrentArea, uint8_t(area));
}

extern "C" void MH3_QuestMapSetup(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestMapSetup", func_803AF518, QuestMapSetupPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803AF518, MH3_QuestMapSetup, "QuestMapSetup");
