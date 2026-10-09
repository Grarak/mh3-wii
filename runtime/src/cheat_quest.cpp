// Cheats in the quests' functions, RMHP08: saving, the arena, the results, the
// faint penalty, the quest's end and the gathering spots. The Gecko codes
// patched instructions of these functions (US address, then EU); here each
// is a wrapper around the translated function that changes its arguments,
// results or the fields it wrote.
#include "cheats.h"

#include "abi_bridge.h"
#include "ppc_runtime.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace {

using Cheats::Activated;
using Cheats::Id;
using Cheats::On;
using Cheats::Toggled;
using Cheats::Value;

// The quest work (r13 - 0x41E0).
constexpr uint32_t kQuestPointer = 0x80797100;

uint32_t Quest() {
    return Memory::Read32(kQuestPointer);
}

// Quest work fields.
enum : uint32_t {
    kObjectives = 0x94,    // u32 flags, 0x60 apart: 8 set, 0x40 failed
    kFaints = 0x91,        // u8
    kReward = 0x2E8,       // s32: the reward money, the quest fails at 0
    kFaintPenalty = 0x2F4, // s32
};

// The gathering spots (r13 - 0x41C8): +0x1C the spots, +0x20 their count.
constexpr uint32_t kSpotsPointer = 0x80797118;
constexpr uint32_t kSpotSize = 0x224;

// Spot fields: u16 counts, 0xFFFF for no limit.
enum : uint32_t {
    kGathersLeft = 0x1FA,
    kCarvesLeft = 0x1FE,
};

// The save data's arena records: 12 best times, and the two hunters' names
// of each (0x34 bytes).
enum : uint32_t {
    kArenaTimes = 0x5334,
    kArenaNames = 0x42D0,
};

constexpr uint32_t kArenaRecords = 12, kArenaNameSize = 0x34;

} // namespace

// 0x80440864 (the save file's writer): fills the save buffer from the save
// data. Can't save (US 0x8044A01C, EU 0x80440E3C: its call when saving
// dropped) leaves the buffer as it was, so a save writes back what was
// loaded.
extern "C" void func_80440864(CpuContext* ctx);

extern "C" void MH3_CheatSaveFill(CpuContext* ctx) {
    if (On(Id::CantSave) && ctx->lr == 0x80440E40)
        return;

    func_80440864(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x80440864, MH3_CheatSaveFill, "CheatSaveFill");

// 0x803975D0 (the quest's result): stores a new arena record in the save
// data. Don't save new arena records (US 0x80397A60 and 0x80397A6C, EU
// 0x80397AC4 and 0x80397AD0: the record's store skipped) puts the records
// back after it.
extern "C" void func_803975D0(CpuContext* ctx);

extern "C" void MH3_CheatArenaRecords(CpuContext* ctx) {
    const uint32_t save = Cheats::UserData();

    if (!On(Id::NoArenaRecords) || save == 0) {
        func_803975D0(ctx);
        return;
    }

    std::array<uint32_t, kArenaRecords> times;
    std::array<uint8_t, kArenaRecords * kArenaNameSize> names;

    for (uint32_t i = 0; i < kArenaRecords; ++i)
        times[i] = Memory::Read32(save + kArenaTimes + 4 * i);

    for (uint32_t i = 0; i < names.size(); ++i)
        names[i] = Memory::Read8(save + kArenaNames + i);

    func_803975D0(ctx);

    for (uint32_t i = 0; i < kArenaRecords; ++i)
        Memory::Write32(save + kArenaTimes + 4 * i, times[i]);

    for (uint32_t i = 0; i < names.size(); ++i)
        Memory::Write8(save + kArenaNames + i, names[i]);
}

REGISTER_NATIVE_FUNCTION_AS(0x803975D0, MH3_CheatArenaRecords, "CheatArenaRecords");

// 0x8039CC9C (the quest result screen's step). Hold to hide rewards (US
// 0x8039CC64, EU 0x8039CCC8: to its end) skips it while the buttons are held.
extern "C" void func_8039CC9C(CpuContext* ctx);

extern "C" void MH3_CheatResultScreen(CpuContext* ctx) {
    if (On(Id::HideRewards) && Activated(Id::HideRewards))
        return;

    func_8039CC9C(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x8039CC9C, MH3_CheatResultScreen, "CheatResultScreen");

// 0x803AF9F0 (player index): a hunter fainted; the reward loses the faint
// penalty (at most all of it) and the faints count up. Infinite retries (US
// 0x803AFA0C, EU 0x803AFA70: sub made add) adds the penalty to the reward
// instead, so it never runs out; Defeats counter always 0 (0x803AFA40/AA4)
// keeps the count at 0.
extern "C" void func_803AF9F0(CpuContext* ctx);

extern "C" void MH3_CheatFaint(CpuContext* ctx) {
    const uint32_t quest = Quest();

    if (quest == 0 || (!On(Id::InfiniteRetries) && !On(Id::NoDefeats))) {
        func_803AF9F0(ctx);
        return;
    }

    const int32_t penalty = int32_t(Memory::Read32(quest + kFaintPenalty));
    const int32_t reward = int32_t(Memory::Read32(quest + kReward));
    const uint8_t faints = Memory::Read8(quest + kFaints);

    // The original takes min(reward, penalty); taking its negative adds it.
    if (On(Id::InfiniteRetries))
        Memory::Write32(quest + kFaintPenalty, uint32_t(-(reward < penalty ? reward : penalty)));

    func_803AF9F0(ctx);

    Memory::Write32(quest + kFaintPenalty, uint32_t(penalty));

    if (On(Id::NoDefeats) && Memory::Read8(quest + kFaints) != faints)
        Memory::Write8(quest + kFaints, 0);
}

REGISTER_NATIVE_FUNCTION_AS(0x803AF9F0, MH3_CheatFaint, "CheatFaint");

// QuestStep (0x803B0228): the quest fails when its reward reaches 0 (state 5
// at +0x6A3C). Autofail (US 0x803B030C, EU 0x803B0370: the reward's check
// dropped), always or while the buttons are held, empties the reward first.
extern "C" void func_803B0228(CpuContext* ctx);

extern "C" void MH3_CheatQuestStep(CpuContext* ctx) {
    const uint32_t quest = Quest();
    const bool fail = On(Id::Autofail) && Activated(Id::Autofail);

    if (fail && quest != 0)
        Memory::Write32(quest + kReward, 0);

    func_803B0228(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x803B0228, MH3_CheatQuestStep, "CheatQuestStep");

// 0x803AA0C4 (quest, objective, flag): 1 when the objective (0 to 3; 4 asks
// about the three) is done: started (8), not failed (0x40) and met (0x20 of
// its second word). Autoclear (US 0x803AA1A4, EU 0x803AA208: the last check
// dropped), always or while the buttons are held, leaves out the last check.
extern "C" void func_803AA0C4(CpuContext* ctx);

extern "C" void MH3_CheatObjectiveDone(CpuContext* ctx) {
    const uint32_t quest = ctx->gpr[3];
    const int32_t objective = int32_t(ctx->gpr[4]);
    const bool clear = On(Id::Autoclear) && Activated(Id::Autoclear);

    if (clear && objective != 4) {
        const uint32_t flags = Memory::Read32(quest + kObjectives + objective * 0x60);

        if ((flags & 8) != 0 && (flags & 0x40) == 0) {
            ctx->gpr[3] = 1;
            return;
        }
    }

    func_803AA0C4(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x803AA0C4, MH3_CheatObjectiveDone, "CheatObjectiveDone");

// 0x80321034 (the quest board's menu): +4 counts up to 6 as the selected
// box jiggles. No quest box jiggle (US 0x80320FFC, EU 0x80321060: the count's
// store dropped) keeps it.
extern "C" void func_80321034(CpuContext* ctx);

extern "C" void MH3_CheatQuestBoard(CpuContext* ctx) {
    const uint32_t menu = ctx->gpr[3];

    if (!On(Id::NoQuestBoxJiggle)) {
        func_80321034(ctx);
        return;
    }

    const uint16_t count = Memory::Read16(menu + 4);
    func_80321034(ctx);

    if (count < 6 && Memory::Read16(menu + 4) == count + 1)
        Memory::Write16(menu + 4, count);
}

REGISTER_NATIVE_FUNCTION_AS(0x80321034, MH3_CheatQuestBoard, "CheatQuestBoard");

// 0x8043E1D8 (the arena's equipment choice): s16 +0x48 counts the time left
// down. Infinite time for choosing your equipment (US 0x80447DFC, EU
// 0x8043EC1C: its store dropped) keeps it.
extern "C" void func_8043E1D8(CpuContext* ctx);

extern "C" void MH3_CheatArenaEquipment(CpuContext* ctx) {
    const uint32_t menu = ctx->gpr[3];

    if (!On(Id::ArenaEquipmentTime)) {
        func_8043E1D8(ctx);
        return;
    }

    const int16_t time = int16_t(Memory::Read16(menu + 0x48));
    func_8043E1D8(ctx);

    if (time > 0 && int16_t(Memory::Read16(menu + 0x48)) == time - 1)
        Memory::Write16(menu + 0x48, uint16_t(time));
}

REGISTER_NATIVE_FUNCTION_AS(0x8043E1D8, MH3_CheatArenaEquipment, "CheatArenaEquipment");

namespace {

// A field of the spots, as it was before the call.
struct SpotCounts {
    uint32_t field;
    std::array<uint16_t, 64> before;
    int32_t count = 0;
};

} // namespace

// 0x803BAC14 (gathering at a spot): the spot's gathers left (and its carves
// left) count down. Inf gathering (US 0x803BAE1E, EU 0x803BAE82: -1 made 0)
// and Inf carving (0x803BAE12/0x803BAE76) keep them: the counts go up by one
// before the call, and the ones it did not take back go down again.
extern "C" void func_803BAC14(CpuContext* ctx);

extern "C" void MH3_CheatGather(CpuContext* ctx) {
    const uint32_t spots = Memory::Read32(kSpotsPointer);

    if ((!On(Id::InfiniteGathering) && !On(Id::InfiniteCarving)) || spots == 0) {
        func_803BAC14(ctx);
        return;
    }

    const uint32_t first = Memory::Read32(spots + 0x1C);
    const int32_t count = std::min<int32_t>(int32_t(Memory::Read32(spots + 0x20)), 64);
    std::array<SpotCounts, 2> kept{SpotCounts{kGathersLeft}, SpotCounts{kCarvesLeft}};
    const std::array<bool, 2> on{On(Id::InfiniteGathering), On(Id::InfiniteCarving)};

    for (size_t k = 0; k < kept.size(); ++k) {
        if (!on[k])
            continue;

        for (int32_t i = 0; i < count; ++i) {
            const uint32_t at = first + i * kSpotSize + kept[k].field;
            const uint16_t left = Memory::Read16(at);
            kept[k].before[i] = left;

            if (left != 0 && left < 0xFFFE)
                Memory::Write16(at, left + 1);
        }

        kept[k].count = count;
    }

    func_803BAC14(ctx);

    for (auto& k : kept) {
        for (int32_t i = 0; i < k.count; ++i) {
            const uint32_t at = first + i * kSpotSize + k.field;
            const uint16_t left = k.before[i];

            if (left != 0 && left < 0xFFFE && Memory::Read16(at) == left + 1)
                Memory::Write16(at, left);
        }
    }
}

REGISTER_NATIVE_FUNCTION_AS(0x803BAC14, MH3_CheatGather, "CheatGather");

// 0x803BAEE4 (a spot's item table: u16 weight, u16 item pairs, 0xFFFF
// ending): picks an item by weight, 0xFFFF for none. Gathering/carving mod
// (US 0x803BAF0C, EU 0x803BAF70: the item's load made li r3,item) picks this
// item.
extern "C" void func_803BAEE4(CpuContext* ctx);

extern "C" void MH3_CheatGatherItem(CpuContext* ctx) {
    func_803BAEE4(ctx);

    if (On(Id::GatherItem) && (ctx->gpr[3] & 0xFFFF) != 0xFFFF)
        ctx->gpr[3] = uint32_t(Value(Id::GatherItem)) & 0xFFFF;
}

REGISTER_NATIVE_FUNCTION_AS(0x803BAEE4, MH3_CheatGatherItem, "CheatGatherItem");

// 0x803BAA14 (player, ...): the spot the hunter can gather or carve at, its
// item kind, 0xFFFF for none. Can't carve/gather (US 0x803BAB44 and
// 0x803BAAA4, EU 0x803BABA8 and 0x803BAB08), switched by its buttons, finds
// none.
extern "C" void func_803BAA14(CpuContext* ctx);

extern "C" void MH3_CheatGatherSpot(CpuContext* ctx) {
    func_803BAA14(ctx);

    if (Toggled(Id::CantGather))
        ctx->gpr[3] = 0xFFFF;
}

REGISTER_NATIVE_FUNCTION_AS(0x803BAA14, MH3_CheatGatherSpot, "CheatGatherSpot");
