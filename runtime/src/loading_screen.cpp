// Loading screen, RMHP08. C reconstruction of the "Now Loading" letter
// animation (0x800D09E4), which DrawHud (0x800D10B0) calls once per frame
// while the loading screen is up.
//
// The letters hop in one after another from the left, hold, then hop out from
// the right, and the cycle repeats. Every call moved the animation one frame
// on (it ran at the Wii's 30 fps); here a call moves it on by the step's
// length in ticks (GameStep::Scale). The sequencing stays on whole frames: as
// a frame begins, a letter may start its hop, a hold counts down or the cycle
// restarts. Through the frame the hopping letters move at their constant
// speed (8 units and 1820 of tilt a frame), so a step that ends inside a
// frame leaves them part of the way. At the stock 1/30 s step a call is one
// frame, as the original.
#include "game_step.h"
#include "memory.h"
#include "port_check.h"
#include "runtime/native/adapters/model_scope_bridge.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace {

// Loading work (0x806977A0). Per letter i: s16 frame of its hop at +0x64 + 2i,
// s16 slide at +0x7A + 2i, s16 height at +0x90 + 2i, s32 tilt at +0x08 + 4i.
constexpr uint32_t kLoading = 0x806977A0;
enum : uint32_t {
    kPhase = 0x02,   // u8: 0 hop in, 1 hold, 2 hop out, 3 hold
    kLead = 0x03,    // s8: last letter that has started its hop
    kLanded = 0x04,  // s8: letters whose hop has finished
    kCounter = 0x06, // s16: frames in this phase (hold: frames left)
    kTilt = 0x08,
    kHop = 0x64,
    kSlide = 0x7A,
    kHeight = 0x90,
};

constexpr uint32_t kLanguage = 0x8065AA69; // u8 (System_w +9)
constexpr int32_t kLetterCounts = -0x7AC0; // r13: u8 letters per language
constexpr int32_t kHopStep = 8, kTiltStep = 1820;
constexpr int16_t kHoldFrames = 4;
constexpr uint32_t kMaxLetters = 11;

int8_t S8(uint32_t o) {
    return int8_t(Memory::Read8(kLoading + o));
}

int16_t S16(uint32_t o) {
    return int16_t(Memory::Read16(kLoading + o));
}

void W8(uint32_t o, int32_t v) {
    Memory::Write8(kLoading + o, uint8_t(v));
}

void W16(uint32_t o, int32_t v) {
    Memory::Write16(kLoading + o, uint16_t(v));
}

// The letters' pose between frames: height, slide, tilt and hop progress
// (frames into the hop, 0..4) of each, and how far into the current frame
// the animation is. The work holds the pose rounded; when it no longer holds
// what was written last (the game set it up again), the pose is read back.
struct Animation {
    uint32_t count = 0;
    std::array<double, kMaxLetters> height{}, slide{}, tilt{}, hop{};
    std::array<bool, kMaxLetters> coming{}; // in the last frame of its hop
    std::array<int32_t, kMaxLetters> shownHeight{}, shownSlide{}, shownTilt{}, shownHop{};
    double into = 0;
    bool valid = false;

    // This frame's hops: letters first..last, sliding by `slideStep` on the
    // way up and tilting by `up` and `down` (the edge letter `still` does not
    // tilt back down); `counts` moves the phase's frame counter on at its end.
    int32_t first = 0, last = -1, slideStep = 0, up = 0, down = 0, still = -1;
    bool counts = false;

    bool Holds() const {
        for (uint32_t i = 0; i < count; ++i)
            if (S16(kHeight + i * 2) != shownHeight[i] || S16(kSlide + i * 2) != shownSlide[i] || int32_t(Memory::Read32(kLoading + kTilt + i * 4)) != shownTilt[i] ||
                S16(kHop + i * 2) != shownHop[i])
                return false;

        return true;
    }

    void ReadBack() {
        for (uint32_t i = 0; i < count; ++i) {
            height[i] = S16(kHeight + i * 2);
            slide[i] = S16(kSlide + i * 2);
            tilt[i] = int32_t(Memory::Read32(kLoading + kTilt + i * 4));
            hop[i] = S16(kHop + i * 2);
        }

        into = 0;
        last = -1;
        counts = false;
    }

    void Write() {
        for (uint32_t i = 0; i < count; ++i) {
            shownHeight[i] = int32_t(std::lround(height[i]));
            shownSlide[i] = int32_t(std::lround(slide[i]));
            shownTilt[i] = int32_t(std::lround(tilt[i]));
            shownHop[i] = int32_t(std::floor(hop[i] + 1e-9));
            W16(kHeight + i * 2, shownHeight[i]);
            W16(kSlide + i * 2, shownSlide[i]);
            Memory::Write32(kLoading + kTilt + i * 4, uint32_t(shownTilt[i]));
            W16(kHop + i * 2, shownHop[i]);
        }
    }

    // Starts the next phase: every letter back to the start of its hop, the
    // pivot letter's tilt set.
    void Restart(uint8_t phase, int8_t lead, uint32_t pivot, int32_t pivotTilt) {
        W8(kLead, lead);
        W8(kPhase, phase);
        W16(kCounter, 1);
        W8(kLanded, 0);
        hop.fill(0);
        tilt[pivot] = pivotTilt;
    }

    // A hold phase: counts the frames left down; false once they are out.
    static bool Holding() {
        const int16_t left = S16(kCounter);

        if (left > 0)
            W16(kCounter, left - 1);

        return left > 0;
    }

    // The frame's sequencing, as the original runs it before the hops.
    void Begin() {
        const int8_t lastLetter = int8_t(count - 1);
        last = -1;
        counts = false;
        coming.fill(false);

        switch (Memory::Read8(kLoading + kPhase)) {
        case 0: // hop in, left to right, a letter every other frame
            if ((S16(kCounter) & 1) == 0) {
                if (S8(kLead) < lastLetter)
                    W8(kLead, S8(kLead) + 1);

                if (S8(kLanded) == int8_t(count)) {
                    W8(kPhase, 1);
                    W16(kCounter, kHoldFrames);
                    return;
                }
            }

            first = 0, last = std::min<int32_t>(lastLetter, S8(kLead));
            slideStep = kHopStep, up = kTiltStep, down = -kTiltStep, still = 0;
            counts = true;
            return;
        case 1:
            if (!Holding())
                Restart(2, lastLetter, count - 1, 3639);

            return;
        case 2: // hop out, right to left
            if ((S16(kCounter) & 1) == 0) {
                if (S8(kLead) > 0)
                    W8(kLead, S8(kLead) - 1);

                if (S8(kLanded) == int8_t(count)) {
                    W8(kPhase, 3);
                    W16(kCounter, kHoldFrames);
                    return;
                }
            }

            first = std::max<int32_t>(0, S8(kLead)), last = lastLetter;
            slideStep = -kHopStep, up = -kTiltStep, down = kTiltStep, still = lastLetter;
            counts = true;
            return;
        case 3:
            if (!Holding())
                Restart(0, 0, 0, 0x10000 - 3641);

            return;
        }
    }

    // `d` of the frame (no further than its end): two frames up, two down.
    void Move(double d) {
        for (int32_t i = first; i <= last; ++i) {
            const double frame = std::floor(hop[i] + 1e-9);

            if (frame <= 1) {
                slide[i] += slideStep * d;
                height[i] -= kHopStep * d;
                tilt[i] += up * d;
            } else if (frame <= 3) {
                height[i] += kHopStep * d;
                tilt[i] += (i == still ? 0 : down) * d;
                coming[i] = frame == 3;
            } else
                continue;

            hop[i] += d;
        }
    }

    // The frame's end: the hops' values are whole again, letters that came
    // down count as landed.
    void End() {
        for (int32_t i = first; i <= last; ++i) {
            hop[i] = std::round(hop[i]);
            height[i] = std::round(height[i]);
            slide[i] = std::round(slide[i]);
            tilt[i] = std::round(tilt[i]);

            if (coming[i])
                W8(kLanded, S8(kLanded) + 1);
        }

        if (counts)
            W16(kCounter, S16(kCounter) + 1);

        last = -1;
    }

    void Step(uint32_t letters, double ticks) {
        if (letters != count || !valid || !Holds()) {
            count = letters;
            ReadBack();
            valid = true;
        }

        for (double left = ticks; left > 1e-9;) {
            if (into == 0)
                Begin();

            const double d = std::min(left, 1.0 - into);
            Move(d);
            into += d;
            left -= d;

            if (into >= 1.0 - 1e-9) {
                into = 0;
                End();
            }
        }

        Write();
    }
};

Animation animation;

void LoadingLetters(CpuContext* ctx) {
    const uint32_t letters = Memory::Read8(ctx->gpr[13] + uint32_t(kLetterCounts) + Memory::Read8(kLanguage));

    if (letters == 0 || letters > kMaxLetters)
        return;

    animation.Step(letters, GameStep::Scale());
}

} // namespace

extern "C" void func_800D09E4(CpuContext* ctx);

extern "C" void MH3_LoadingLetters(CpuContext* ctx) {
    PortCheck::Call(ctx, "LoadingLetters", func_800D09E4, LoadingLetters);

    // The loading screen absorbs the area's first-sight work: resolve the
    // draw tables of the models loaded so far.
    PrepareNativeModelDrawTables();
}
REGISTER_NATIVE_FUNCTION_AS(0x800D09E4, MH3_LoadingLetters, "LoadingLetters");
