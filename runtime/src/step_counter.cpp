// The step counter, RMHP08. RunTasks (0x800416F8) adds 1 to system_w+0xC every
// logic step, and 280-odd places in the game read it, almost all as periodic
// tests: `counter % N == k` runs something every N steps (AI checks, spawns,
// effects), `counter % N < k` or `counter & bit` is a duty cycle (blinking),
// counter << s is a phase (sines).
//
// Under variable steps the counter counts whole ticks of game time instead:
// before RunTasks adds its 1, the counter is moved on by Ticks() - 1. That makes
// duty cycles and phases follow game time. The periodic equality tests are
// what is left: on steps shorter than a tick the counter does not change, so a
// test that passed would pass again, and on longer steps it skips values. The
// translator hooks those loads (scripts/step_counter_sites.py lists them with
// their N and k; recomp.yml translation.load_hooks_path) and they read here
// instead: the tick of this step that is k mod N if the tick count passed one,
// otherwise a value that fails the test. Every N ticks the test passes on
// exactly one step, whatever the step length. At the stock 1/30 s step (one
// tick per step) every read is the counter itself. The player's step counter
// (PLW+0x20, a tick at a time) gets the same for its tests in code that runs
// once a step (kind 2 sites).
#include "game_step.h"
#include "step_counter.h"
#include "port_check.h"
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"
#include "ppc_runtime.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iterator>

namespace {

constexpr uint32_t kCounter = 0x8065AA6C;       // system_w+0xC: u32 steps
constexpr uint32_t kPlaySteps = 0x8065B2F4;     // system_w+0x894: u32 steps of play time
constexpr uint32_t kPlaySeconds = 0x8065B2F0;   // system_w+0x890: u32 seconds of play time
constexpr uint32_t kPlaySecondsMax = 35999999;  // 9999:59:59
constexpr uint32_t kVillage = 0x806ACFC4;       // village scene work
constexpr uint32_t kVillageSteps = kVillage + 0x30;       // u32
constexpr uint32_t kVillageCountdown = kVillage + 0x28;   // u8
constexpr uint32_t kVillageRamp = kVillage + 0x34;        // s16: steps in the current state, +-60
constexpr uint32_t kVillageTalk = 0x806ACD48;   // village talk work
constexpr uint32_t kVillageTalkSteps = kVillageTalk + 0x150; // u32
// Steps in the area, advanced by AreaStepCounters (0x802E4C88, below).
constexpr uint32_t kAreaSteps[] = {0x806C04E8, 0x806C02D8, 0x806C046C};
// Quest clock steps (quest work+0x28), advanced by 0x803B0228 (tick sites).
constexpr uint32_t kQuestSteps = 0x806C7D00;

bool IsCounter(uint32_t address) {
    return address == kCounter || address == kPlaySteps || address == kVillageSteps || address == kVillageTalkSteps ||
           address == kQuestSteps ||
           std::find(std::begin(kAreaSteps), std::end(kAreaSteps), address) != std::end(kAreaSteps);
}

struct Site {
    uint32_t pc;
    uint32_t n; // 0: N is a register and the test is == 0
    uint32_t k;
    uint32_t kind; // 0: any other use; 1: `% N == k`; 2: the same on a player's step counter
};
constexpr Site kSites[] = {
#include "step_counter_sites.inc"
};

const Site* Find(uint32_t pc) {
    const auto* it = std::lower_bound(std::begin(kSites), std::end(kSites), pc,
                                      [](const Site& s, uint32_t v) { return s.pc < v; });
    return it != std::end(kSites) && it->pc == pc ? it : nullptr;
}

// MH3_STEP_COUNTER_RAW=1 (debugging): the counter counts steps and every read
// is the counter itself, as the original.
bool Raw() {
#ifdef MH3_DEVELOPMENT
    static const bool raw = std::getenv("MH3_STEP_COUNTER_RAW") != nullptr;
    return raw;
#else
    return false;
#endif
}

} // namespace

extern "C" uint32_t MH3_HookedLoad32(uint32_t base, int32_t offset, uint32_t pc) {
    const uint32_t address = base + uint32_t(offset);
    const uint32_t counter = Memory::Read32(address);
    const Site* site = Find(pc);
    const uint32_t ticks = GameStep::Ticks();
    // A player's step counter (PLW+0x20, advanced a tick at a time by
    // player_timers.cpp) is read through the PLW, so its sites (kind 2) name it.
    if (!site || ticks == 1 || site->kind == 0 || Raw()) return counter;
    if (site->kind == 1 && !IsCounter(address)) return counter;
    if (site->n == 0) return ticks == 0 ? 1u : counter; // 1 % N != 0 for any N > 1
    // The latest value in (counter - ticks, counter] that is k mod N.
    const uint32_t back = (counter % site->n + site->n - site->k % site->n) % site->n;
    if (back < ticks) return counter - back;
    return counter % site->n == site->k ? counter + 1 : counter;
}

bool StepCounter::Every(uint32_t n, uint32_t k) {
    const uint32_t counter = Memory::Read32(kCounter), ticks = GameStep::Ticks();
    if (n == 0) return counter == k; // the guest's divwu by 0 gives 0: counter - 0 * 0
    if (ticks == 1 || Raw()) return counter % n == k;
    // Whether a value in (counter - ticks, counter] is k mod n.
    return (counter % n + n - k % n) % n < ticks;
}

// RunTasks: the counter advances by the ticks of this step (RunTasks adds 1).
extern "C" void func_800416F8(CpuContext* ctx);
extern "C" void MH3_RunTasks(CpuContext* ctx) {
    if (!Raw()) Memory::Write32(kCounter, Memory::Read32(kCounter) + GameStep::Ticks() - 1);
    func_800416F8(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x800416F8, MH3_RunTasks, "RunTasks");

// The play-time clock (0x800D2F80, called by each scene's step): steps, and
// seconds every 30 of them. One pass per tick.
static void PlayTimeTickPort(CpuContext*) {
    for (uint32_t i = 0, ticks = GameStep::Ticks(); i < ticks; ++i) {
        const uint32_t steps = Memory::Read32(kPlaySteps) + 1;
        Memory::Write32(kPlaySteps, steps);
        if (steps % 30 != 0) continue;
        const uint32_t seconds = Memory::Read32(kPlaySeconds) + 1;
        Memory::Write32(kPlaySeconds, seconds > kPlaySecondsMax ? kPlaySecondsMax : seconds);
    }
}
extern "C" void func_800D2F80(CpuContext* ctx);
extern "C" void MH3_PlayTimeTick(CpuContext* ctx) { PortCheck::Call(ctx, "PlayTimeTick", func_800D2F80, PlayTimeTickPort); }
REGISTER_NATIVE_FUNCTION_AS(0x800D2F80, MH3_PlayTimeTick, "PlayTimeTick");

// The village's step counters (0x80212680, called by the village step
// 0x801FC750): a countdown, a ramp that counts steps towards +60 or -60 with
// the state of 0x8021F29C, and the step counter. One pass per tick.
static void VillageStepCountersPort(CpuContext* ctx) {
    const CpuContext saved = *ctx;
    const uint32_t ticks = GameStep::Ticks();
    for (uint32_t i = 0; i < ticks; ++i)
        if (const uint8_t n = Memory::Read8(kVillageCountdown)) Memory::Write8(kVillageCountdown, n - 1);
    const bool down = GuestCall{ctx}(0x8021F29C) != 0;
    int32_t ramp = int16_t(Memory::Read16(kVillageRamp));
    if (down) {
        if (ramp > 0) ramp = 0;
        ramp = std::max(ramp - int32_t(ticks), std::min(ramp, -60));
    } else {
        if (ramp < 0) ramp = 0;
        ramp = std::min(ramp + int32_t(ticks), std::max(ramp, 60));
    }
    Memory::Write16(kVillageRamp, uint16_t(ramp));
    Memory::Write32(kVillageSteps, Memory::Read32(kVillageSteps) + ticks);
    *ctx = saved;
}
extern "C" void func_80212680(CpuContext* ctx);
extern "C" void MH3_VillageStepCounters(CpuContext* ctx) { PortCheck::Call(ctx, "VillageStepCounters", func_80212680, VillageStepCountersPort); }
REGISTER_NATIVE_FUNCTION_AS(0x80212680, MH3_VillageStepCounters, "VillageStepCounters");

// The village talk step (0x8021DC88) adds 1 to its step counter: here the
// ticks of this step.
extern "C" void func_8021DC88(CpuContext* ctx);
extern "C" void MH3_VillageTalkStep(CpuContext* ctx) {
    Memory::Write32(kVillageTalkSteps, Memory::Read32(kVillageTalkSteps) + GameStep::Ticks() - 1);
    func_8021DC88(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x8021DC88, MH3_VillageTalkStep, "VillageTalkStep");

// AreaStepCounters (0x802E4C88) adds 1 to each of the area's step counters
// every step: here the ticks of this step, as the step counter itself.
static void AreaStepCountersPort(CpuContext*) {
    for (const uint32_t steps : kAreaSteps) Memory::Write32(steps, Memory::Read32(steps) + GameStep::Ticks());
}
extern "C" void func_802E4C88(CpuContext* ctx);
extern "C" void MH3_AreaStepCounters(CpuContext* ctx) {
    PortCheck::Call(ctx, "AreaStepCounters", func_802E4C88, AreaStepCountersPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x802E4C88, MH3_AreaStepCounters, "AreaStepCounters");
