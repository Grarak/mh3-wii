// Loading screen, RMHP08. C reconstruction of the "Now Loading" letter
// animation (0x800D09E4), which DrawHud (0x800D10B0) calls once per frame
// while the loading screen is up.
//
// The letters hop in one after another from the left, hold, then hop out from
// the right, and the cycle repeats. Every call moves the animation one frame
// on, so it ran at the Wii's 30 fps; the body therefore runs once per whole
// tick of game time elapsed this step (GameStep::Ticks), as the player timers
// do. At the stock 1/30 s step it runs once, as the original.
#include "game_step.h"
#include "memory.h"
#include "port_check.h"
#include "runtime/native/adapters/model_scope_bridge.h"

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

struct Letters {
    uint32_t count;
    int8_t S8(uint32_t o) const { return int8_t(Memory::Read8(kLoading + o)); }
    int16_t S16(uint32_t o) const { return int16_t(Memory::Read16(kLoading + o)); }
    void W8(uint32_t o, int32_t v) const { Memory::Write8(kLoading + o, uint8_t(v)); }
    void W16(uint32_t o, int32_t v) const { Memory::Write16(kLoading + o, uint16_t(v)); }
    void Add16(uint32_t o, int32_t v) const { W16(o, S16(o) + v); }
    void AddTilt(uint32_t i, int32_t v) const { Memory::Write32(kLoading + kTilt + i * 4, Memory::Read32(kLoading + kTilt + i * 4) + uint32_t(v)); }

    // One frame of letter i's hop: two frames up (tilting by `tiltUp`), two
    // frames down (tilting by `tiltDown`); it slides by `slide` on the way up.
    void Hop(uint32_t i, int32_t slide, int32_t tiltUp, int32_t tiltDown) const {
        const int16_t frame = S16(kHop + i * 2);
        if (uint16_t(frame) <= 1) {
            Add16(kSlide + i * 2, slide);
            Add16(kHeight + i * 2, -kHopStep);
            AddTilt(i, tiltUp);
            Add16(kHop + i * 2, 1);
        } else if (frame == 2 || frame == 3) {
            Add16(kHeight + i * 2, kHopStep);
            AddTilt(i, tiltDown);
            Add16(kHop + i * 2, 1);
            if (S16(kHop + i * 2) == 4)
                W8(kLanded, S8(kLanded) + 1);
        }
    }

    // Starts the next phase: every letter back to the start of its hop.
    void Restart(uint8_t phase, int8_t lead, uint32_t tiltAt, uint32_t tilt) const {
        W8(kLead, lead);
        W8(kPhase, phase);
        W16(kCounter, 1);
        W8(kLanded, 0);
        for (uint32_t i = 0; i < count; ++i)
            W16(kHop + i * 2, 0);
        Memory::Write32(kLoading + tiltAt, tilt);
    }

    // A hold phase: counts the frames left down; false once they are out.
    bool Holding() const {
        const int16_t left = S16(kCounter);
        if (left > 0)
            W16(kCounter, left - 1);
        return left > 0;
    }

    void Frame() const {
        const int8_t last = int8_t(count - 1);
        switch (Memory::Read8(kLoading + kPhase)) {
        case 0: // hop in, left to right, a letter every other frame
            if ((S16(kCounter) & 1) == 0) {
                if (S8(kLead) < last)
                    W8(kLead, S8(kLead) + 1);
                if (S8(kLanded) == int8_t(count)) {
                    W8(kPhase, 1);
                    W16(kCounter, kHoldFrames);
                    return;
                }
            }
            for (int32_t i = 0; i < int32_t(count) && i <= S8(kLead); ++i)
                Hop(uint32_t(i), kHopStep, kTiltStep, i == 0 ? 0 : -kTiltStep);
            W16(kCounter, S16(kCounter) + 1);
            return;
        case 1:
            if (!Holding())
                Restart(2, last, kTilt - 4 + count * 4, 3639);
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
            for (int32_t i = int32_t(count) - 1; i >= 0 && i >= S8(kLead); --i)
                Hop(uint32_t(i), -kHopStep, -kTiltStep, i == int32_t(count) - 1 ? 0 : kTiltStep);
            W16(kCounter, S16(kCounter) + 1);
            return;
        case 3:
            if (!Holding())
                Restart(0, 0, kTilt, 0x10000 - 3641);
            return;
        }
    }
};

void LoadingLetters(CpuContext* ctx) {
    const Letters letters{Memory::Read8(ctx->gpr[13] + uint32_t(kLetterCounts) + Memory::Read8(kLanguage))};
    for (uint32_t i = 0, ticks = GameStep::Ticks(); i < ticks; ++i)
        letters.Frame();
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
