// The scene's world matrix pass, RMHP08, with the root's models split between
// the game thread and the guest worker. nwMoveEnd (0x800D5D90), at the end of
// the field task, runs ScnRoot::CalcWorld (0x800832F4): the root's G3dProc
// with task 1 walks the scene, and each model calculates its world matrices
// from its animation (2.85 ms/frame on the devkit in the Alatreon fight, 17
// models). Decompiled with Ghidra:
//
//   0x800832F4 ScnRoot::CalcWorld(root): 0x80083180(root), then the root's
//     G3dProc (vtable +0x0C) with task 1, no argument and no information.
//   0x80081E50, task 1 of ScnGroup::G3dProc (0x80082100): the group's
//     callback 1, its own world matrix (vtable +0x30, given the information
//     and the address of the argument, which it may replace), callback 2, its
//     world matrix (0x8007B5A8), task 1 on each child (+0xDC children, +0xE4
//     count) with the argument and that matrix, then callback 4.
//
// The root's children are separate models (the stage, the hunter, the
// hunter's equipment, the monster). While the root's children run, the worker
// takes some of them, chosen by their last measured cost so both sides take
// about as long, and the game thread the rest, each side in the original
// order. The models share neither the locked cache, which is never granted
// (LockedCacheAcquire, g3d_calc_world.cpp), nor the scratch at 0x8066D300,
// which the worker has a copy of. MH3_PORT_CHECK=ScnGroupCalcWorld compares
// the root's split pass with the original in one thread (MH3_VARIABLE_STEP=0;
// the worker's stack and scratch, which the original leaves alone, differ).
// The split is on in every build. The MH3 world split line (development)
// shows each side's time.
#include "abi_bridge.h"
#include "guest_call.h"
#include "guest_worker.h"
#include "memory.h"
#include "port_check.h"
#include <algorithm>
#include <array>
#include <chrono>
#ifdef MH3_DEVELOPMENT
#include <cstdio>
#endif
#ifdef __vita__
#include <psp2/kernel/processmgr.h>
#endif

namespace {

enum : uint32_t {
    Callback = 0x8007B5C8,    // (object, timing, argument, information)
    WorldMatrix = 0x8007B5A8, // (object, 1): the object's world matrix
    RootPrepare = 0x80083180, // (root)
};

enum : uint32_t {
    kG3dProc = 0x0C,   // vtable: G3dProc(object, task, argument, information)
    kCalcWorld = 0x30, // vtable: the object's own world matrix (object, information, &argument)
    kChildren = 0xDC,
    kChildCount = 0xE4,
    kCalcWorldCallback = 0x11C, // ScnMdlSimple: the CalcWorld callback object
    kCalcWorldTiming = 0x120,   // u8: when it runs (0: never)
    kCharacterModel = 0x118,    // MHchar: its ScnMdlSimple
};

// CalcWorld callback classes (vtables).
enum : uint32_t {
    kArmorCallback = 0x805BB334,   // the hunter's armor parts
    kWeaponCallback = 0x805BB318,  // the hunter's weapon parts
    kMonsterCallback = 0x805A1B28, // a monster's own bones
};

constexpr uint32_t kTaskCalcWorld = 1;
constexpr unsigned kMaxChildren = 64;

// The root whose ScnRoot::CalcWorld is running, 0 otherwise.
uint32_t worldRoot = 0;

uint64_t NowUs() {
#ifdef __vita__
    return sceKernelGetProcessTimeWide();
#else
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

// The last measured cost of each child, which decides the split.
struct Cost {
    uint32_t child = 0;
    uint32_t us = 0;
};

std::array<Cost, kMaxChildren> costs{};
unsigned costCount = 0;

uint32_t LastCost(uint32_t child) {
    for (unsigned i = 0; i < costCount; ++i) {
        if (costs[i].child == child)
            return costs[i].us;
    }

    // Not seen yet: a typical small model.
    return 50;
}

// One side's children, in the original order, and their measured costs.
struct Share {
    uint32_t argument = 0, matrix = 0;
    unsigned count = 0;
    std::array<uint32_t, kMaxChildren> children{};
    std::array<uint32_t, kMaxChildren> us{};
};

Share workerShare, gameShare;
#ifdef MH3_DEVELOPMENT
// The MH3 world split line: per split pass, each side's time (from the
// submit to its last model) and the largest model.
struct SplitStatistics {
    uint64_t passes = 0, gameUs = 0, workerUs = 0, largestUs = 0, models = 0;
} statistics;
#endif

void RunShare(CpuContext* cpu, Share& share) {
    GuestCall call{cpu};

    for (unsigned i = 0; i < share.count; ++i) {
        const uint64_t start = NowUs();
        call.Virtual(share.children[i], kG3dProc, {kTaskCalcWorld, share.argument, share.matrix});
        share.us[i] = uint32_t(NowUs() - start);
    }
}

void RunWorkerShare(CpuContext* cpu, void*) {
    RunShare(cpu, workerShare);
}

// In every build: the overlapped draw reads the matrices on its own core
// either way, and the split gains about 2 logical fps (docs/guest-worker.md).
bool SplitAllowed() {
#ifdef MH3_DEVELOPMENT
    // Port checks compare whole RAM around their own call and need the game
    // thread's order; ScnGroupCalcWorld checks the split pass itself.
    if (PortCheck::Set() && !PortCheck::Enabled("ScnGroupCalcWorld"))
        return false;
#endif
    return GuestWorker::Available() && !GuestWorker::Pending();
}

// The models that must stay on the game thread, in their order. A model's
// CalcWorld callback (ScnMdlSimple +0x11C, called at a node during its world
// matrices) runs game code:
// - the hunter's equipment parts (0x802296FC, 0x80229A84) copy bones of the
//   hunter's body model into their own nodes, reading the body's matrices of
//   the previous pass (the parts come first in the root's order): the parts
//   and that body stay together, in order;
// - a monster's (0x801398BC, 0x80139BD0) moves only its own bones;
// - a callback whose timing mask is 0 never runs;
// - any other callback may read anything and stays on the game thread.
void PinDependentModels(const uint32_t* children, uint32_t count, bool* pinned) {
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t callback = Memory::Read32(children[i] + kCalcWorldCallback);

        if (!callback)
            continue;

        const uint32_t vtable = Memory::Read32(callback);

        if (vtable == kArmorCallback || vtable == kWeaponCallback) {
            pinned[i] = true;
            const uint32_t character = Memory::Read32(callback + (vtable == kArmorCallback ? 8 : 4));
            const uint32_t body = character ? Memory::Read32(character + kCharacterModel) : 0;

            for (uint32_t j = 0; j < count; ++j)
                pinned[j] = pinned[j] || (body && children[j] == body);
        } else if (vtable != kMonsterCallback && Memory::Read8(children[i] + kCalcWorldTiming) != 0) {
            pinned[i] = true;
        }
    }
}

// Task 1 on the root's children: split by last cost (the longest first, each
// to the side with less so far), each side keeping the original order.
void SplitChildren(CpuContext* cpu, uint32_t group, uint32_t argument, uint32_t matrix) {
    const uint32_t count = Memory::Read32(group + kChildCount), list = Memory::Read32(group + kChildren);
    std::array<uint32_t, kMaxChildren> children{};
    std::array<uint8_t, kMaxChildren> order{}, side{};

    for (uint32_t i = 0; i < count; ++i) {
        children[i] = Memory::Read32(list + i * 4);
        order[i] = uint8_t(i);
    }

    std::stable_sort(order.begin(), order.begin() + count, [&](uint8_t a, uint8_t b) { return LastCost(children[a]) > LastCost(children[b]); });
    uint64_t load[2] = {0, 0};

    std::array<bool, kMaxChildren> pinned{};
    PinDependentModels(children.data(), count, pinned.data());

    for (uint32_t i = 0; i < count; ++i) {
        if (pinned[i])
            load[0] += LastCost(children[i]);
    }

    for (uint32_t i = 0; i < count; ++i) {
        if (pinned[order[i]])
            continue;

        const unsigned to = load[1] < load[0] ? 1 : 0;
        side[order[i]] = uint8_t(to);
        load[to] += LastCost(children[order[i]]);
    }

    workerShare.count = gameShare.count = 0;
    workerShare.argument = gameShare.argument = argument;
    workerShare.matrix = gameShare.matrix = matrix;

    for (uint32_t i = 0; i < count; ++i) {
        Share& share = side[i] ? workerShare : gameShare;
        share.children[share.count++] = children[i];
    }

    if (workerShare.count)
        GuestWorker::Submit(*cpu, RunWorkerShare, nullptr);

    RunShare(cpu, gameShare);
    GuestWorker::Wait();
    costCount = 0;
#ifdef MH3_DEVELOPMENT
    uint32_t sum[2] = {0, 0}, largest = 0;
#endif

    for (const Share* share : {&gameShare, &workerShare}) {
        for (unsigned i = 0; i < share->count; ++i) {
            costs[costCount++] = {share->children[i], share->us[i]};
#ifdef MH3_DEVELOPMENT
            sum[share == &workerShare] += share->us[i];
            largest = std::max(largest, share->us[i]);
#endif
        }
    }
#ifdef MH3_DEVELOPMENT
    ++statistics.passes;
    statistics.gameUs += sum[0];
    statistics.workerUs += sum[1];
    statistics.largestUs += largest;
    statistics.models += count;
#endif
}

void GroupCalcWorld(CpuContext* cpu) {
    const uint32_t group = cpu->gpr[3], argument = cpu->gpr[4], information = cpu->gpr[5];
    GuestFrame frame(cpu, 0x30);
    GuestCall call{cpu};
    const uint32_t local = frame[0x08];
    Memory::Write32(local, argument);
    call.Call<Callback>({group, 1, argument, information});
    call.Virtual(group, kCalcWorld, {information, local});
    call.Call<Callback>({group, 2, Memory::Read32(local), information});
    const uint32_t matrix = call.Call<WorldMatrix>({group, 1});

    if (group == worldRoot && Memory::Read32(group + kChildCount) <= kMaxChildren && SplitAllowed()) {
        SplitChildren(cpu, group, Memory::Read32(local), matrix);
    } else {
        // The count and the argument are read again for each child, as in the original.
        for (uint32_t i = 0; i < Memory::Read32(group + kChildCount); ++i)
            call.Virtual(Memory::Read32(Memory::Read32(group + kChildren) + i * 4), kG3dProc, {kTaskCalcWorld, Memory::Read32(local), matrix});
    }

    call.Call<Callback>({group, 4, Memory::Read32(local), information});
}

void RootCalcWorld(CpuContext* cpu) {
    const uint32_t root = cpu->gpr[3];
    GuestFrame frame(cpu, 0x10);
    GuestCall call{cpu};
    call.Call<RootPrepare>({root});
    const uint32_t previous = std::exchange(worldRoot, root);
    call.Virtual(root, kG3dProc, {kTaskCalcWorld, 0, 0});
    worldRoot = previous;
}

} // namespace

extern "C" void func_80081E50(CpuContext* cpu);
extern "C" void MH3_ScnGroupCalcWorld(CpuContext* cpu) {
    // Only the root's pass is checked: the groups below it may run on the
    // worker, and a check saves and restores all of RAM.
    if (cpu->gpr[3] == worldRoot)
        PortCheck::Call(cpu, "ScnGroupCalcWorld", func_80081E50, GroupCalcWorld);
    else
        GroupCalcWorld(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80081E50, MH3_ScnGroupCalcWorld, "ScnGroupCalcWorld");

extern "C" void func_800832F4(CpuContext* cpu);
extern "C" void MH3_ScnRootCalcWorld(CpuContext* cpu) {
    PortCheck::Call(cpu, "ScnRootCalcWorld", func_800832F4, RootCalcWorld);
}
REGISTER_NATIVE_FUNCTION_AS(0x800832F4, MH3_ScnRootCalcWorld, "ScnRootCalcWorld");

#ifdef MH3_DEVELOPMENT
void MH3PrintWorldSplit(unsigned frames) {
    if (!frames)
        return;

    const auto ms = [&](uint64_t us) { return double(us) / 1000.0 / frames; };
    std::printf("MH3 world split: %.2f passes/frame, models on the game thread %.2f ms, on the guest worker %.2f ms, the largest model %.2f ms per frame; %.1f models/pass\n",
                double(statistics.passes) / frames, ms(statistics.gameUs), ms(statistics.workerUs), ms(statistics.largestUs),
                statistics.passes ? double(statistics.models) / double(statistics.passes) : 0.0);
    statistics = {};
}
#endif
