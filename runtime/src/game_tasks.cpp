// Game tasks, RMHP08. C ports of the task starts (decompiled from the
// translation).
//
// RunTasks (0x800416F8) runs 16 task slots of 32 bytes at 0x8065B5D0: +0 u16
// flags (8: new), +4 the task function; the task keeps its state in the rest
// (VillageTask: +8 phase, +9 state, +10 sub-state). The scene tasks (title,
// village, field) run in slot 4.
#include "game_tasks.h"

#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"

namespace {

constexpr uint32_t kSystem = 0x8065AA60;
enum : uint32_t {
    kGameMode = 0x24,       // u8: 1 field, 2 village
    kVillageVariant = 0x7D3, // u8: 1 starts 0x803A1418 instead of the village task
    kFieldFlag = 0x868,      // u8: cleared when the field starts
    kExitHook = 0x904,       // u32: a function the scene leaves to run on exit
};
constexpr uint32_t kTaskSlots = 0x8065B5D0, kTaskSize = 32;
constexpr uint16_t kTaskNew = 8;
enum : uint32_t { kSceneSlot = 4 };
enum : uint32_t {
    FieldTask = 0x8028BF80,
    VillageTask = 0x8021F40C,
    VillageVariantTask = 0x803A1418,
};

} // namespace

// 0x80041930 StartTask(function, s16 slot): the slot restarts with `function`.
void GameTasks::StartTask(uint32_t function, int16_t slot) {
    const uint32_t task = kTaskSlots + uint32_t(int32_t(slot)) * kTaskSize;
    for (uint32_t offset = 0; offset < kTaskSize; offset += 4) Memory::Write32(task + offset, 0);
    Memory::Write16(task, kTaskNew);
    Memory::Write32(task + 4, function);
}

extern "C" void func_80041930(CpuContext* ctx);
static void StartTaskPort(CpuContext* ctx) { GameTasks::StartTask(ctx->gpr[3], int16_t(ctx->gpr[4])); }

extern "C" void MH3_StartTask(CpuContext* ctx) { PortCheck::Call(ctx, "StartTask", func_80041930, StartTaskPort); }
REGISTER_NATIVE_FUNCTION_AS(0x80041930, MH3_StartTask, "StartTask");

// 0x800419DC StartGameModeTask(): starts the scene task of the game mode.
void GameTasks::StartGameModeTask() {
    switch (Memory::Read8(kSystem + kGameMode)) {
    case 1:
        Memory::Write8(kSystem + kFieldFlag, 0);
        StartTask(FieldTask, kSceneSlot);
        break;
    case 2:
        StartTask(Memory::Read8(kSystem + kVillageVariant) == 1 ? VillageVariantTask : VillageTask, kSceneSlot);
        break;
    }
}

extern "C" void func_800419DC(CpuContext* ctx);
static void StartGameModeTaskPort(CpuContext*) { GameTasks::StartGameModeTask(); }

extern "C" void MH3_StartGameModeTask(CpuContext* ctx) { PortCheck::Call(ctx, "StartGameModeTask", func_800419DC, StartGameModeTaskPort); }
REGISTER_NATIVE_FUNCTION_AS(0x800419DC, MH3_StartGameModeTask, "StartGameModeTask");

// 0x800408B8 RunExitHook(): runs system_w+0x904 if set (a tail call: it gets
// the caller's registers).
extern "C" void func_800408B8(CpuContext* ctx);
static void RunExitHookPort(CpuContext* ctx) {
    const uint32_t hook = Memory::Read32(kSystem + kExitHook);
    if (!hook) return;
    ctx->ctr = hook;
    InvokeIndirectCpu(hook, ctx);
}

extern "C" void MH3_RunExitHook(CpuContext* ctx) { PortCheck::Call(ctx, "RunExitHook", func_800408B8, RunExitHookPort); }
REGISTER_NATIVE_FUNCTION_AS(0x800408B8, MH3_RunExitHook, "RunExitHook");
