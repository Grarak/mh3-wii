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
// then loads that area. --area n replaces it.
//
// --spawn x,y,z moves the hunter there once the quest has placed them (the
// player's move work, kind 2 in the table 0x8065AB04, position at +0x3C) and
// that placement has held for kSpawnSettle frames: the field places the hunter
// more than once while it starts. The arena's start area is its waiting room;
// --spawn 0,300,0 drops the hunter into the pit.
//
// The village runs 30 steps in its main state before leaving.
#include "field_jump.h"

#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"

#include <cstdio>
#include <cstdlib>

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
constexpr uint32_t MoveWork = 0x800CFAF4;       // get_move_work_adrs(u8 kind) -> move work
constexpr uint32_t kCurrentArea = 0x806BB806;   // u8, get_now_areano
constexpr uint32_t kMoveWorkTable = 0x8065AB04; // +16: move works by kind
constexpr uint32_t kPlayerMoveWork = 2;
enum : uint32_t {
    kPosition = 0x3C
};
constexpr uint64_t kSpawnSettle = 60; // game frames (30 Hz)
enum : uint32_t {
    kWorkArea = 0xEA,
    kWorkStartArea = 0xEE
};
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
    uint64_t placedAt = 0; // game frame the hunter was first seen placed, 0: not yet
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

void FieldJumpFrame(uint64_t gameFrame) {
    Jump& jump = State();
    if (!jump.spawn || !jump.done)
        return;
    const uint32_t table = Memory::Read32(kMoveWorkTable);
    const uint32_t plw = table ? Memory::Read32(table + 16 + kPlayerMoveWork * 4) : 0;
    if (!plw || !Memory::Read8(plw) || Memory::ReadFloat32(plw + kPosition) == 0.0f) {
        jump.placedAt = 0;
        return;
    }
    if (!jump.placedAt)
        jump.placedAt = gameFrame;
    if (gameFrame - jump.placedAt < kSpawnSettle)
        return;
    for (int i = 0; i < 3; ++i)
        Memory::WriteFloat32(plw + kPosition + 4 * i, jump.spawnPos[i]);
    std::fprintf(stderr, "MH3 field jump: moved the hunter to %.1f %.1f %.1f\n", jump.spawnPos[0], jump.spawnPos[1], jump.spawnPos[2]);
    jump.spawn = false;
}

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

// The quest's map setup; then the chosen start area.
extern "C" void func_803AF518(CpuContext* ctx);
static void QuestMapSetupPort(CpuContext* ctx) {
    func_803AF518(ctx);
    const Jump& jump = State();
    if (!jump.done || jump.area < 0)
        return;
    const CpuContext saved = *ctx;
    GuestCall game{ctx};
    const uint32_t work = game.Call<MoveWork>({0});
    *ctx = saved;
    if (!work)
        return;
    std::fprintf(stderr, "MH3 field jump: starting in area %d instead of %u\n", jump.area, Memory::Read8(work + kWorkArea));
    Memory::Write8(work + kWorkArea, uint8_t(jump.area));
    Memory::Write8(work + kWorkStartArea, uint8_t(jump.area));
    Memory::Write8(kCurrentArea, uint8_t(jump.area));
}

extern "C" void MH3_QuestMapSetup(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestMapSetup", func_803AF518, QuestMapSetupPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803AF518, MH3_QuestMapSetup, "QuestMapSetup");
