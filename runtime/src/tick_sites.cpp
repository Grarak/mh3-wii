// Tick sites, RMHP08. Many of the game's step counters are `x + 1` and its
// countdowns `x - 1` inline in code that is otherwise independent of the step
// length. The translator passes the value loaded at each listed site
// (tick_sites.inc) through MH3_TickLoad, which moves it on by the ticks of this
// step minus the one the guest adds or takes: the stored result has advanced
// by GameStep::Ticks(). At the stock 1/30 s step (one tick) every value is
// returned as loaded.
//
// Eases: `x += (goal - x) / d` each step closes 1/d of the gap per step. Float
// sites (EaseDivisor) load such a d: MH3_EaseDivisor returns the divisor that
// closes the same gap over this step's length s, 1 / (1 - (1 - 1/d)^s).
// PerStep sites load an amount the guest applies once per step (a rate):
// MH3_PerStep scales it by s. Decay sites load a factor f of `x *= f` each
// step: MH3_Decay returns f^s.
//
// Advance sites load an integer the guest moves on by a constant each step (a
// spin angle, a phase): the site's limit is that amount k, and the value is
// moved on so that the store advances it by GameStep::Share(k), k * s.
// Rate sites load that amount itself (a speed kept in memory): it is replaced
// by its share of this step, GameStep::Share(amount).
//
// CountupEvery / CountdownEvery sites are counters the guest moves by 1 and
// then tests with `% N == 0` (limit N: a sound every 32 steps). The count
// moves by the ticks of this step, but lands on the multiple of N it passed,
// so the test passes once per N ticks; a step without a tick that would store
// the same multiple again moves the count 1 on instead (the test must fail).
#include "game_step.h"
#include "ppc_runtime.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#ifdef MH3_DEVELOPMENT
#include <chrono>
#include <unordered_map>
#include <vector>
#endif

namespace {

enum class TickSite : uint32_t {
    Countdown,         // x - 1 while x > limit (0): stops at limit
    CountdownPastZero, // x - 1 until x < 0
    Countup,           // x + 1
    EaseDivisor,       // f32 d of x += (goal - x) / d (MH3_EaseDivisor)
    PerStep,           // f32 amount applied once per step (MH3_PerStep)
    Decay,             // f32 factor f of x *= f (MH3_Decay)
    Advance,           // x + limit: moved on by limit * s
    Rate,              // the amount k of x + k: k * s
    CountupEvery,      // x + 1, then tested % limit == 0
    CountdownEvery,    // x - 1, then tested % limit == 0
    Survey,            // development: record the site, return the value (limit: the guest's +-1)
};

struct Site {
    uint32_t pc;
    TickSite kind;
    // Countup: the largest value the guest still adds 1 to (0: none).
    // Countdown: the value the guest counts down to (x - 1 while x > limit).
    // Advance: the amount the guest adds each step.
    // CountupEvery / CountdownEvery: the N of the % N == 0 test.
    int32_t limit = 0;
};
constexpr Site kSites[] = {
#include "tick_sites.inc"
};

const Site* Find(uint32_t pc) {
    const auto* it = std::lower_bound(std::begin(kSites), std::end(kSites), pc, [](const Site& s, uint32_t v) { return s.pc < v; });
    return it != std::end(kSites) && it->pc == pc ? it : nullptr;
}

#ifdef MH3_DEVELOPMENT
// Survey sites (a build that lists candidate `x +- 1` loads as Survey, for
// finding step counters): MH3_TICK_SURVEY=file gets "pc execs steps continued
// maxPerStep" per site every 10 s. `continued` counts loads of a value this
// site stored in the previous step: a per-step counter continues about once
// per step, a loop index or an event count does not.
struct SurveyEntry {
    uint64_t execs = 0, steps = 0, continued = 0, maxPerStep = 0, perStep = 0, lastStep = 0;
    std::vector<uint32_t> prev, cur;
};
std::unordered_map<uint32_t, SurveyEntry> survey;

void SurveyDump() {
    const char* path = std::getenv("MH3_TICK_SURVEY");
    if (!path)
        return;
    FILE* f = std::fopen(path, "w");
    if (!f)
        return;
    for (const auto& [pc, e] : survey)
        std::fprintf(f, "%08x %llu %llu %llu %llu\n", pc, (unsigned long long)e.execs, (unsigned long long)e.steps, (unsigned long long)e.continued, (unsigned long long)e.maxPerStep);
    std::fclose(f);
}

void SurveyRecord(uint32_t pc, uint32_t value, int32_t imm) {
    static double key = -1;
    static uint64_t step = 0;
    static auto last = std::chrono::steady_clock::now();
    if (GameStep::Elapsed() != key) {
        key = GameStep::Elapsed();
        ++step;
        if (std::chrono::steady_clock::now() - last > std::chrono::seconds(10)) {
            last = std::chrono::steady_clock::now();
            SurveyDump();
        }
    }
    SurveyEntry& e = survey[pc];
    if (e.lastStep != step) {
        if (e.lastStep + 1 == step)
            e.prev.swap(e.cur);
        else
            e.prev.clear();
        e.cur.clear();
        e.lastStep = step;
        ++e.steps;
        e.perStep = 0;
    }
    ++e.execs;
    e.maxPerStep = std::max(e.maxPerStep, ++e.perStep);
    if (std::find(e.prev.begin(), e.prev.end(), value) != e.prev.end())
        ++e.continued;
    if (e.cur.size() < 64)
        e.cur.push_back(value + uint32_t(imm));
}
#endif

} // namespace

extern "C" uint32_t MH3_TickLoad(uint32_t value, uint32_t pc) {
    const int32_t ticks = int32_t(GameStep::Ticks());
    const Site* site = Find(pc);
    if (site && site->kind == TickSite::Advance) {
        return value + uint32_t(GameStep::Share(site->limit) - site->limit);
    }
    if (site && site->kind == TickSite::Rate)
        return uint32_t(GameStep::Share(int32_t(value)));
    if (site && site->kind == TickSite::Survey) {
#ifdef MH3_DEVELOPMENT
        SurveyRecord(pc, value, site->limit);
#endif
        return value;
    }
    if (!site || ticks == 1)
        return value;
    const int32_t v = int32_t(value);
    if (site->kind == TickSite::CountupEvery || site->kind == TickSite::CountdownEvery) {
        const int64_t n = site->limit;
        const auto mod = [n](int64_t x) {
            return (x % n + n) % n;
        };
        const int64_t step = site->kind == TickSite::CountupEvery ? 1 : -1;
        int64_t target;
        if (ticks == 0) {
            target = mod(v) == 0 ? v + step : v;
        } else {
            target = v + step * ticks;
            // The multiple of N passed first (nearest v), if one was passed.
            const int64_t first = step > 0 ? v + 1 + mod(-(v + 1)) : v - 1 - mod(v - 1);
            if (mod(target) != 0 && (step > 0 ? first < target : first > target))
                target = first;
        }
        return uint32_t(int32_t(target - step));
    }
    switch (site->kind) {
    case TickSite::Countdown:
        return v > site->limit ? uint32_t(std::max(v - ticks, site->limit) + 1) : value;
    case TickSite::CountdownPastZero:
        return uint32_t(v - ticks + 1);
    case TickSite::EaseDivisor:
    case TickSite::PerStep:
    case TickSite::Decay:
    case TickSite::Advance:
    case TickSite::Rate:
    case TickSite::CountupEvery:
    case TickSite::CountdownEvery:
    case TickSite::Survey:
        return value;
    case TickSite::Countup:
        if (site->limit != 0 && v > site->limit)
            return value;
        return uint32_t(site->limit != 0 ? std::min(v + ticks - 1, site->limit) : v + ticks - 1);
    }
    return value;
}

extern "C" double MH3_EaseDivisor(double d) {
    const double s = GameStep::Scale();
    if (s == 1.0 || !(d > 1.0))
        return d;
    const double closed = 1.0 - std::pow(1.0 - 1.0 / d, s);
    return closed > 0.0 ? 1.0 / closed : HUGE_VAL;
}

extern "C" double MH3_PerStep(double amount) {
    return amount * GameStep::Scale();
}

extern "C" double MH3_Decay(double factor) {
    const double s = GameStep::Scale();
    return s == 1.0 || !(factor > 0.0) ? factor : std::pow(factor, s);
}
