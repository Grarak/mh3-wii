// MH3_TRACE=stall (development builds): finds steps of game time in which the
// hunter and the large monsters stand frozen while the game itself runs on (a
// player report: everyone froze for seconds, effects kept playing). Once a
// frame outside loading screens it takes a signature of each actor (its
// motion state: channel rates and frames, the pause and hold counts; its
// position) and prints
//
//   MH3 stall: all actors ...   when none of them changed for 15 ticks of
//                               game time (0.5 s), with what holds actors in
//                               the original: the system pause (0x8065AA90,
//                               which skips the field's moves; the picture
//                               stays still, dimmed, effects too), the event
//                               demo state (0x806D4FA0: 4 plays, 2-3 load and
//                               prepare its streams), the event state
//                               (0x806C13A0: the tutorial panel, which sets
//                               the system pause while it shows), each
//                               actor's action and its pause (hit stop) and
//                               hold counts;
//   MH3 stall: <actor> ...      when one of them did not change for 30 ticks;
//   MH3 stall: ... ends         with the game time and frames it lasted.
#include "stall_watch.h"

#ifdef MH3_DEVELOPMENT
#include "cheats.h"
#include "dev_options.h"
#include "game_step.h"
#include "load_progress.h"
#include "memory.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

constexpr uint32_t kSystemPause = 0x8065AA90; // u8: 0x80046F70 returns it, the field skips its moves
constexpr uint32_t kOtherPause = 0x8065AA65;  // u8: native_scene_feedback.cpp's second pause test
constexpr uint32_t kDemoState = 0x806D4FA0;   // u8 (event_demo.cpp), +2 the demo
constexpr uint32_t kEventState = 0x806C13A0;  // u8: the tutorial panel's state (0x8033A1C4)
constexpr uint32_t kBaseRate = 0x80793628;    // f32 nw4r::g3d::FrameCtrl::smBaseUpdateRate
constexpr uint32_t kAreaNo = 0x806BB806;      // u8: get_now_areano

// The motion state mhchar_motion.cpp's fields are relative to, the object the
// MHchar update (0x800E078C) is given: 4 bytes into the hunter's MHchar
// (PLW +0x13C), at +0x24 of a monster work.
constexpr uint32_t kHunterChar = 0x13C, kHunterPosition = 0x3C, kMonsterMotion = 0x24, kMonsterPosition = 0x188;
constexpr uint32_t kMotion = 0x50, kFrames = 0x74, kPause = 0xF1, kHold = 0xF2;

uint32_t HunterMotion(uint32_t plw) {
    return Memory::Read32(plw + kHunterChar) + 4;
}

// Ticks of game time a freeze must last to be reported.
constexpr double kAllTicks = 15, kOneTicks = 30;

constexpr uint32_t kActors = 9; // the hunter, then up to 8 monster works

struct Actor {
    uint32_t work = 0;
    uint64_t signature = 0;
    double since = 0; // game time of its last change
    uint64_t frame = 0;
    bool reported = false;
};

std::array<Actor, kActors> actors;
double allSince = 0;
uint64_t allFrame = 0, frames = 0;
bool allReported = false;

uint64_t Mix(uint64_t h, uint32_t v) {
    return (h ^ v) * 0x100000001B3ull;
}

// The motion's rates, frames and previous frames, the pause and hold counts,
// and the position.
uint64_t Signature(uint32_t motion, uint32_t position) {
    uint64_t h = 0xCBF29CE484222325ull;

    for (uint32_t a = motion + 0x5C; a < motion + 0xA4; a += 4)
        h = Mix(h, Memory::Read32(a));

    h = Mix(h, Memory::Read16(motion + kPause));

    for (uint32_t k = 0; k < 3; ++k)
        h = Mix(h, Memory::Read32(position + 4 * k));

    return h;
}

std::string Describe(uint32_t index, uint32_t work) {
    char text[200];

    if (index == 0) {
        const uint32_t chr = HunterMotion(work);
        std::snprintf(text, sizeof text, "hunter action %u.%u.%u state %u motion %u frame %.2f pause %u hold %u at %.0f %.0f %.0f", Memory::Read8(work + 9), Memory::Read8(work + 10),
                      Memory::Read16(work + 12), Memory::Read8(work + 5), Memory::Read16(chr + kMotion), Memory::ReadFloat32(chr + kFrames), Memory::Read8(chr + kPause),
                      Memory::Read8(chr + kHold), Memory::ReadFloat32(work + kHunterPosition), Memory::ReadFloat32(work + kHunterPosition + 4),
                      Memory::ReadFloat32(work + kHunterPosition + 8));
    } else {
        const uint32_t chr = work + kMonsterMotion;
        std::snprintf(text, sizeof text, "monster %u kind %u area %u action %u.%u state %u.%u active %u halted %u motion %u frame %.2f pause %u hold %u", index - 1, Memory::Read8(work + 3),
                      Memory::Read8(work + 0x1E1), Memory::Read8(work + 0x1E5), Memory::Read8(work + 0x1E6), Memory::Read8(work + 5), Memory::Read8(work + 6), Memory::Read8(work + 0x1DE),
                      Memory::Read8(work + 0x46A), Memory::Read16(chr + kMotion), Memory::ReadFloat32(chr + kFrames), Memory::Read8(chr + kPause), Memory::Read8(chr + kHold));
    }

    return text;
}

void PrintGlobals(const char* what) {
    std::fprintf(stderr, "MH3 stall: %s at game %.2f s frame %llu: system pause %u/%u demo state %u id %u event %u area %u base rate %.3f step %.3f\n", what, GameStep::Elapsed() / 30.0,
                 (unsigned long long)frames, Memory::Read8(kSystemPause), Memory::Read8(kOtherPause), Memory::Read8(kDemoState), Memory::Read8(kDemoState + 2), Memory::Read8(kEventState),
                 Memory::Read8(kAreaNo), Memory::ReadFloat32(kBaseRate), GameStep::Scale());
}

} // namespace

void StallWatchFrame() {
    static const TraceFlag on("stall");

    if (!on)
        return;

    ++frames;
    const double now = GameStep::Elapsed();
    const uint32_t plw = Cheats::LocalPlayer();

    if (LoadProgress::Showing() || !plw || !Memory::Read32(plw + kHunterChar)) {
        actors = {};
        allSince = now;
        allFrame = frames;
        allReported = false;
        return;
    }

    std::array<uint32_t, kActors> works{plw};

    for (uint32_t i = 0, n = Cheats::MoveWorkCount(Cheats::Kind::Monster); i < n && i + 1 < kActors; ++i)
        if (const uint32_t m = Cheats::MoveWork(Cheats::Kind::Monster, i); m && Memory::Read16(m) == 0x0101)
            works[i + 1] = m;

    bool anyChanged = false, any = false;

    for (uint32_t i = 0; i < kActors; ++i) {
        Actor& a = actors[i];

        if (!works[i]) {
            a = {};
            continue;
        }

        any = true;
        const uint64_t signature = i == 0 ? Signature(HunterMotion(plw), plw + kHunterPosition) : Signature(works[i] + kMonsterMotion, works[i] + kMonsterPosition);

        if (a.work != works[i] || a.signature != signature) {
            if (a.reported)
                std::fprintf(stderr, "MH3 stall: %s ends after %.2f s game time, %llu frames\n", i == 0 ? "hunter" : ("monster " + std::to_string(i - 1)).c_str(),
                             (now - a.since) / 30.0, (unsigned long long)(frames - a.frame));

            anyChanged |= a.work == works[i];
            a = {works[i], signature, now, frames, false};
            continue;
        }

        if (!a.reported && now - a.since >= kOneTicks) {
            a.reported = true;
            PrintGlobals((i == 0 ? std::string("hunter") : "monster " + std::to_string(i - 1)).c_str());
            std::fprintf(stderr, "MH3 stall:   %s\n", Describe(i, works[i]).c_str());
        }
    }

    if (!any || anyChanged) {
        if (allReported)
            std::fprintf(stderr, "MH3 stall: all actors end after %.2f s game time, %llu frames\n", (now - allSince) / 30.0, (unsigned long long)(frames - allFrame));

        allSince = now;
        allFrame = frames;
        allReported = false;
        return;
    }

    if (!allReported && now - allSince >= kAllTicks) {
        allReported = true;
        PrintGlobals("all actors");

        for (uint32_t i = 0; i < kActors; ++i)
            if (works[i])
                std::fprintf(stderr, "MH3 stall:   %s\n", Describe(i, works[i]).c_str());
    }
}
#endif
