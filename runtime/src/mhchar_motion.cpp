// MHchar root motion, RMHP08. C reconstruction of the character model's
// per-step animation update (0x800E19D4, called by MHchar::move 0x800E078C),
// decompiled with Ghidra (scripts/dol2elf.py -> ELF import).
//
// MHchar plays up to six NW4R g3d animation channels. Each step it saves the
// channel frames, advances them (the channel set's UpdateFrame; NW4R adds
// rate * FrameCtrl::smBaseUpdateRate per channel), wraps channels past their
// end to the loop start, and sums each channel's root-motion displacement,
// weighted by its blend weight, into the character position.
//
// The original measures the displacement over `rate` frames, the channel's
// nominal per-step advance, starting at the new frame (one step ahead), so
// characters moved the same distance per step at any step length. Here it
// spans rate * smBaseUpdateRate, the frames the animation actually advanced,
// ending the same one nominal step ahead, so consecutive windows join up. This
// is identical at the stock 1/30 s step (base rate 1.0) and makes movement
// follow game time at any step length.
#include "game_step.h"
#include "port_check.h"
#include "guest_call.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>
#ifdef MH3_DEVELOPMENT
#include <cstdio>
#include <cstdlib>
#endif

namespace {

constexpr uint32_t kAnimationBaseRate = 0x80793628; // f32 nw4r::g3d::FrameCtrl::smBaseUpdateRate

// MHchar fields (this = the MHchar).
enum : uint32_t {
    kPosition = 0x04,    // VEC3
    kScale = 0x1C,       // VEC3: root motion scale
    kRotation = 0x28,    // rotation applied to root motion (rotVecZXY)
    kFlags = 0x4C,       // u32: bit 2 = no root motion
    kBlendLeft = 0x44,   // f32: frames of the cross-fade to the new motion left
    kBlendLength = 0x48, // f32: the cross-fade's length in frames
    kRate = 0x5C,        // f32[6]: channel frames per step (speed * 2.0)
    kFrame = 0x74,       // f32[6]
    kPrevFrame = 0x8C,   // f32[6]: frames before this step
    kEndFrame = 0xA4,    // f32[6]
    kLoopFrame = 0xBC,   // f32[6]: < 0 = hold the last frame
    kSkipAdvance = 0xF0, // u8: frames were just set; don't advance this step
    kPauseSteps = 0xF1,  // u8: steps to skip entirely (hit stop)
    kHoldSteps = 0xF2,   // u8: steps to advance without moving
    kRootNode = 0xEC,    // u32: the node whose translation is the root motion
    kWeight = 0xD4,      // f32[6]: channel blend weights before the cross-fade
    kFadingTag = 0x100,  // u32[3]: cleared with the fading channels 3-5
    kKeptAnims = 0x11C,  // u32[16]: animations the character keeps (not released)
    kChannelSet = 0x160, // channel set object (AnmObj with per-channel weights)
};

enum : uint32_t {
    ChannelAnim = 0x800E1C90,       // (this, channel) -> AnmObj* or 0
    ReleaseAnim = 0x8007B3AC,       // (AnmObj*)
    Hermite = 0x800522EC,           // (f1 t, f2..f7 curve) -> f1
    InitAnmResult = 0x800610CC,     // (buffer)
    VecScale = 0x80051F44,          // (out, in; f1 scale)
    VecAdd = 0x80073FCC,            // (inout, add)
    RotVecZXY = 0x80051278,         // (inout VEC3*, rotation)
    ResultTranslation = 0x8008E224, // (const ChrAnmResult*, out VEC3*)
};

// AnmObj virtual slots (kDetach and the weights: the channel set's).
enum : uint32_t {
    kSetFrame = 0x1C,
    kGetFrame = 0x20,
    kUpdateFrame = 0x24,
    kSetRate = 0x28,
    kGetResult = 0x38,
    kDetach = 0x40,
    kSetWeight = 0x48,
    kGetWeight = 0x4C,
};
constexpr uint32_t kZero = 0x807988F8, kOne = 0x807988FC; // f32 0 and 1 (r2-30312, r2-30308)

float F32(uint32_t address) {
    return Memory::ReadFloat32(address);
}

// The pause and hold counters count ticks of game time. A tick can be part
// used up when a step ends: the part is kept here, with the count it belongs
// to (a count the game sets anew starts from a whole tick).
struct Partial {
    uint8_t count = 0;
    double used = 0;
};
// A channel's root-node translation at the frame its last motion window ended
// on: the next window starts there.
struct RootSample {
    uint32_t anim = 0; // 0: none
    float frame = 0, end = 0;
    std::array<float, 3> at{};
};
struct Counters {
    Partial pause, hold;
    std::array<RootSample, 6> roots;
};
std::unordered_map<uint32_t, Counters> counters;

// Spends up to `time` ticks on the u8 count at `address`; returns the time
// left over once it has run out (0 while it still runs).
double Spend(uint32_t address, Partial& partial, double time) {
    uint8_t count = Memory::Read8(address);
    if (count != partial.count)
        partial.used = 0;
    while (count > 0 && time > 0) {
        const double need = 1.0 - partial.used;
        if (time >= need) {
            time -= need;
            partial.used = 0;
            --count;
        } else {
            partial.used += time;
            time = 0;
        }
    }
    Memory::Write8(address, count);
    partial.count = count;
    return count > 0 ? 0 : time;
}

// The root node's translation in channel animation `anim` at `frame`, through
// the guest's result buffer `result` and VEC3 `out`; leaves the frame set.
std::array<float, 3> RootAt(GuestCall& game, uint32_t self, uint32_t anim, float frame, uint32_t result, uint32_t out) {
    game.Virtual<kSetFrame>(anim, {}, {frame});
    const uint32_t evaluated = game.Virtual<kGetResult>(anim, {result, Memory::Read32(self + kRootNode)});
    game.Call<ResultTranslation>({evaluated, out});
    return {F32(out), F32(out + 4), F32(out + 8)};
}

// 0x800E1D2C (MHcharRootMotionDelta): the channel's root translation at `to`
// minus at `from` (zero without an animation), the channel's frame restored.
// `known` is the translation at `from` when the caller has it.
std::array<float, 3> RootDelta(GuestCall& game, uint32_t self, uint32_t anim, float from, float to, uint32_t result, uint32_t out, const std::array<float, 3>* known, std::array<float, 3>* atTo) {
    game.Call<InitAnmResult>({result});
    game.Virtual<kGetFrame>(anim);
    const float frame = game.F1();
    const std::array<float, 3> a = known ? *known : RootAt(game, self, anim, from, result, out);
    const std::array<float, 3> b = RootAt(game, self, anim, to, result, out);
    if (atTo)
        *atTo = b;
    game.Virtual<kSetFrame>(anim, {}, {frame});
    return {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
}

// 0x800E1C90 (MHcharChannelAnim): channel i's animation, or 0. The channel set
// holds a count (+0x18) and an array of AnmObj pointers (+0x1C); an index out
// of range panics (the original does that through 0x80501130).
uint32_t Channel(GuestCall& game, uint32_t self, uint32_t i) {
    const uint32_t set = Memory::Read32(self + kChannelSet);
    if (i >= Memory::Read32(set + 0x18)) [[unlikely]]
        return game.Call<ChannelAnim>({self, i});
    return Memory::Read32(Memory::Read32(set + 0x1C) + 4 * i);
}

// Sets the weights of channels first..first+2 to their shares of `share`, in
// proportion to their weights at the start of the cross-fade (kWeight); those
// without an animation get 0.
void Weigh(GuestCall& game, uint32_t self, uint32_t first, float share) {
    const float zero = F32(kZero);
    float total = zero;
    for (uint32_t ch = first; ch < first + 3; ++ch)
        if (Channel(game, self, ch))
            total += F32(self + kWeight + 4 * ch);
    for (uint32_t ch = first; ch < first + 3; ++ch) {
        const float weight = Channel(game, self, ch) && total > zero ? share * (F32(self + kWeight + 4 * ch) / total) : zero;
        game.Virtual<kSetWeight>(Memory::Read32(self + kChannelSet), {ch}, {weight});
    }
}

// 0x800E1EB4 (MHcharChannelBlend), once per step after the channels advanced.
// Channels 0-2 play the character's motion; while it cross-fades from the one
// before, that one plays on in channels 3-5. The cross-fade's frames left fall
// by channel 0's rate each step; the old motion's share of the blend eases
// (Hermite) from 1 to 0 over the cross-fade's length. When it runs out, the old
// channels are detached (their animations released unless the character keeps
// them) and the new ones take the whole blend.
//
// Here the frames left fall by the rate times the step length, so a cross-fade
// takes the same time at any step length. At the stock 1/30 s step this is
// the original.
void BlendChannels(GuestCall& game, uint32_t self) {
    if (!Memory::Read32(self + kChannelSet))
        return;
    const float zero = F32(kZero), one = F32(kOne);
    float old = zero; // the old motion's share
    if (Channel(game, self, 3)) {
        const float left = F32(self + kBlendLeft) - F32(self + kRate) * float(GameStep::Scale());
        Memory::WriteFloat32(self + kBlendLeft, left);
        if (left <= zero) {
            Memory::WriteFloat32(self + kBlendLeft, zero);
            for (uint32_t ch = 0; ch < 3; ++ch) {
                const uint32_t anim = game.Virtual<kDetach>(Memory::Read32(self + kChannelSet), {ch + 3});
                Memory::Write32(self + kFadingTag + 4 * ch, 0);
                bool kept = false;
                for (uint32_t k = 0; k < 16 && anim; ++k)
                    kept |= Memory::Read32(self + kKeptAnims + 4 * k) == anim;
                if (anim && !kept)
                    game.Call<ReleaseAnim>({anim});
                if (const uint32_t now = Channel(game, self, ch))
                    game.Virtual<kSetRate>(now, {}, {F32(self + kRate + 4 * ch)});
            }
            Memory::Write32(self + kFlags, Memory::Read32(self + kFlags) & ~3u);
            Weigh(game, self, 0, one);
            return;
        }
        const float length = F32(self + kBlendLength);
        if (!(left > length)) {
            game.Call<Hermite>({}, {left / length, zero, zero, zero, one, one, zero});
            old = game.F1();
        }
        Weigh(game, self, 3, old);
    }
    Weigh(game, self, 0, one - old);
}

} // namespace

static void MHcharChannelBlendPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 80);
    GuestCall game{ctx};
    BlendChannels(game, frame.Arg(3));
}
extern "C" void func_800E1EB4(CpuContext* ctx);
extern "C" void MH3_MHcharChannelBlend(CpuContext* ctx) {
    PortCheck::Call(ctx, "MHcharChannelBlend", func_800E1EB4, MHcharChannelBlendPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x800E1EB4, MH3_MHcharChannelBlend, "MHcharChannelBlend");

static void MHcharRootMotionDeltaPort(CpuContext* ctx) {
    const uint32_t self = ctx->gpr[3], out = ctx->gpr[4], channelIndex = ctx->gpr[5];
    const float from = float(ctx->fpr[1].d), to = float(ctx->fpr[2].d);
    GuestFrame frame(ctx, 0xC0);
    GuestCall game{ctx};
    const uint32_t anim = game.Call<ChannelAnim>({self, channelIndex});
    std::array<float, 3> d{};
    if (anim)
        d = RootDelta(game, self, anim, from, to, frame[0x2C], frame[0x14], nullptr, nullptr);
    for (uint32_t k = 0; k < 3; ++k)
        Memory::WriteFloat32(out + k * 4, d[k]);
}
extern "C" void func_800E1D2C(CpuContext* ctx);
extern "C" void MH3_MHcharRootMotionDelta(CpuContext* ctx) {
    PortCheck::Call(ctx, "MHcharRootMotionDelta", func_800E1D2C, MHcharRootMotionDeltaPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x800E1D2C, MH3_MHcharRootMotionDelta, "MHcharRootMotionDelta");

static void MHcharRootMotionPort(CpuContext* ctx) {
    const CpuContext saved = *ctx;
    const uint32_t self = saved.gpr[3];
    // Guest frame as the original's (0xB0): VEC3s at sp+8/+20/+32, result
    // buffer at sp+44; below it the root delta's result buffer and VEC3.
    const uint32_t sp = (saved.gpr[1] - 0x110) & ~0xFu;
    ctx->gpr[1] = sp;
    Memory::Write32(sp, saved.gpr[1]);
    GuestCall game{ctx};
    const uint32_t scaled = sp + 8, delta = sp + 20, sum = sp + 32, result = sp + 44;
    const uint32_t rootResult = sp + 0xB0, rootOut = sp + 0x100;
    auto channel = [&](uint32_t i, uint32_t field) {
        return self + field + i * 4;
    };

    const uint32_t channelSet = Memory::Read32(self + kChannelSet);
    if (!channelSet || !game.Call<ChannelAnim>({self, 0})) {
        *ctx = saved;
        return;
    }
    // The pause (hit stop) and hold counters count ticks of game time (the
    // original: 1 per step), to the exact time within the step: the animation
    // only advances for the part of the step after the pause, and the
    // character only moves for the part after the hold.
    const double step = GameStep::Scale();
    Counters& state = counters[self];
    const bool pausing = Memory::Read8(self + kPauseSteps) != 0;
    const double animated = pausing ? Spend(self + kPauseSteps, state.pause, step) : step;
    if (animated <= 0) {
        *ctx = saved;
        return;
    }
    // The part of the step after a pause: the animation's base rate is the
    // step's length, so it advances by that part here.
    const float stepRate = F32(kAnimationBaseRate);
    if (animated != step)
        Memory::WriteFloat32(kAnimationBaseRate, float(stepRate * (animated / step)));
    game.Call<InitAnmResult>({result});
    for (uint32_t i = 0; i < 6; ++i)
        Memory::Write32(channel(i, kPrevFrame), Memory::Read32(channel(i, kFrame)));
#ifdef MH3_DEVELOPMENT
    // MH3_ANIM_WHOLE_FRAMES=1 (debugging): the channels are shown at whole
    // frames; their exact frames are kept here and restored before advancing.
    static const bool wholeFrames = std::getenv("MH3_ANIM_WHOLE_FRAMES") != nullptr;
    static std::unordered_map<uint64_t, float> exact;
    if (wholeFrames)
        for (uint32_t i = 0; i < 6; ++i) {
            const uint32_t anim = game.Call<ChannelAnim>({self, i});
            if (!anim)
                continue;
            game.Virtual<kGetFrame>(anim);
            const float shown = game.F1();
            const auto it = exact.find(uint64_t(self) << 8 | i);
            if (it != exact.end() && std::floor(it->second) == shown)
                game.Virtual<kSetFrame>(anim, {}, {it->second});
        }
#endif
    if (Memory::Read8(self + kSkipAdvance) == 0) {
        game.Virtual<kUpdateFrame>(channelSet);
    } else {
        // Frames were just set: the last windows' ends say nothing about them.
        Memory::Write8(self + kSkipAdvance, 0);
        state.roots = {};
    }
#ifdef MH3_DEVELOPMENT
    if (wholeFrames)
        for (uint32_t i = 0; i < 6; ++i) {
            const uint32_t anim = game.Call<ChannelAnim>({self, i});
            if (!anim)
                continue;
            game.Virtual<kGetFrame>(anim);
            const float frameNow = game.F1();
            exact[uint64_t(self) << 8 | i] = frameNow;
            game.Virtual<kSetFrame>(anim, {}, {std::floor(frameNow)});
        }
#endif
    BlendChannels(game, self);

    const float zero = 0.0f; // f32 at r2-30312 (0x807988F8)
    for (uint32_t k = 0; k < 3; ++k)
        Memory::WriteFloat32(sum + k * 4, zero);
    const float baseRate = F32(kAnimationBaseRate);
    for (uint32_t i = 0; i < 6; ++i) {
        const uint32_t anim = game.Call<ChannelAnim>({self, i});
        if (!anim)
            continue;
        game.Virtual<kGetFrame>(anim);
        Memory::WriteFloat32(channel(i, kFrame), game.F1());
        if (F32(channel(i, kFrame)) > F32(channel(i, kEndFrame))) {
            if (F32(channel(i, kLoopFrame)) < zero) {
                Memory::WriteFloat32(channel(i, kFrame), F32(channel(i, kEndFrame)));
            } else {
                game.Virtual<kSetFrame>(anim, {}, {F32(channel(i, kLoopFrame))});
                game.Virtual<kGetFrame>(anim);
                Memory::WriteFloat32(channel(i, kFrame), game.F1());
            }
        }
        // Frames the channel advanced this step (the original: rate alone),
        // measured, as the original's, one nominal step (rate) ahead of the
        // frame: [frame + rate - span, frame + rate]. Consecutive windows then
        // join up at any step length.
        const float rate = F32(channel(i, kRate));
        const float span = rate * baseRate;
        game.Virtual<kGetFrame>(anim);
        const float frame = game.F1();
        float from = span == rate ? frame : frame + (rate - span), to = frame + rate;
        if (to >= F32(channel(i, kEndFrame))) {
            to = frame;
            from = frame - span;
        }
        if (from < zero)
            from = zero;
        if (to < zero)
            to = zero;
        // The translation at `from` is the last window's end when this window
        // starts there (to within rounding: both are sums of the same frames).
        RootSample& root = state.roots[i];
        const float end = F32(channel(i, kEndFrame));
        const bool joined = root.anim == anim && root.end == end && std::fabs(root.frame - from) <= 1e-5f * std::max(1.0f, std::fabs(from));
#ifdef MH3_DEVELOPMENT
        // MH3_ROOT_CACHE_CHECK=1 (debugging): evaluates the joined window's
        // start afresh too and reports the largest difference seen.
        static const bool checkRoots = std::getenv("MH3_ROOT_CACHE_CHECK") != nullptr;
        if (checkRoots && joined) {
            static uint64_t checked = 0;
            static float worst = 0;
            game.Call<InitAnmResult>({rootResult});
            game.Virtual<kGetFrame>(anim);
            const float kept = game.F1();
            const std::array<float, 3> fresh = RootAt(game, self, anim, from, rootResult, rootOut);
            game.Virtual<kSetFrame>(anim, {}, {kept});
            for (uint32_t k = 0; k < 3; ++k)
                worst = std::max(worst, std::fabs(fresh[k] - root.at[k]));
            ++checked;
            if ((checked & (checked - 1)) == 0 && checked >= 256)
                std::fprintf(stderr, "MH3 root cache: %llu joined windows, largest start difference %g\n", static_cast<unsigned long long>(checked), double(worst));
        }
#endif
        std::array<float, 3> atTo;
        const std::array<float, 3> d = RootDelta(game, self, anim, from, to, rootResult, rootOut, joined ? &root.at : nullptr, &atTo);
        root = {anim, to, end, atTo};
        for (uint32_t k = 0; k < 3; ++k)
            Memory::WriteFloat32(delta + k * 4, d[k]);
        game.Virtual<kGetWeight>(channelSet, {i});
        game.Call<VecScale>({scaled, delta}, {game.F1()});
        game.Call<VecAdd>({sum, scaled});
    }
    Memory::WriteFloat32(kAnimationBaseRate, stepRate);
#ifdef MH3_DEVELOPMENT
    // MH3_MOTION_LOG=file: per call, the game time, the character, its
    // position and its first two channels' frame and rate.
    static FILE* log = [] {
        const char* path = std::getenv("MH3_MOTION_LOG");
        return path ? std::fopen(path, "w") : nullptr;
    }();
    if (log)
        std::fprintf(log, "%.4f %08x %.2f %.2f %.2f %.3f %.3f %.3f %.3f\n", GameStep::Elapsed(), self, F32(self + kPosition), F32(self + kPosition + 4), F32(self + kPosition + 8),
                     F32(channel(0, kFrame)), F32(channel(0, kRate)), F32(channel(1, kFrame)), F32(channel(1, kRate)));
#endif
    const bool holding = Memory::Read8(self + kHoldSteps) != 0;
    const double moved = holding ? Spend(self + kHoldSteps, state.hold, animated) : animated;
    if (moved > 0 && (Memory::Read32(self + kFlags) & 2) == 0) { // 0x800E19BC, inlined
        // After a hold ends within the step, only the motion of the rest of it.
        const float part = moved == animated ? 1.0f : float(moved / animated);
        for (uint32_t k = 0; k < 3; ++k)
            Memory::WriteFloat32(sum + k * 4, F32(sum + k * 4) * F32(self + kScale + k * 4) * part);
        game.Call<RotVecZXY>({sum, self + kRotation});
        game.Call<VecAdd>({self + kPosition, sum});
    }
    *ctx = saved;
}
extern "C" void func_800E19D4(CpuContext* ctx);
extern "C" void MH3_MHcharRootMotion(CpuContext* ctx) {
    PortCheck::Call(ctx, "MHcharRootMotion", func_800E19D4, MHcharRootMotionPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x800E19D4, MH3_MHcharRootMotion, "MHcharRootMotion");
