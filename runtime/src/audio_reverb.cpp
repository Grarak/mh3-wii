// Three-channel reverb, RMHP08. C reconstruction of the AX auxiliary callback
// 0x80475880 (AudioThreeChannelFilterCallback), decompiled from the
// disassembly.
//
// Each call processes one AX frame, 96 samples of three channels (left, right,
// surround), in place. Per channel and sample:
//   in      = the channel's sample (+ the aux input's, when one is set);
//   early   = three taps of the channel's early-reflection line (the input
//             goes in at the third tap);
//   x       = the input through the channel's pre-delay line (when one is set);
//   comb    = the sum of three comb filters of x, each fed back by its gain;
//   the sum goes through two allpass filters, a one-pole lowpass and a third,
//   per-channel allpass; wet = early + that * 0.6 * level.
// The channels are then cross-mixed (each gets 0.5 * crosstalk of the other
// two), scaled by the output gain and written back as integers; with a send
// buffer set, also scaled by the send gain into it. All delay-line positions
// then move on by one sample.
//
// The math keeps the PPC single-precision rounding of the translated code, so
// the output matches it bit for bit; the port saves the per-access guest
// memory checks and the register spills, and keeps the delay-line positions,
// gains and lowpass state in registers for the frame.
#include "abi_bridge.h"
#include "memory.h"
#include "port_check.h"
#include "ppc_runtime.h"

#include <cstring>

namespace {

// Reverb state (r4).
enum : uint32_t {
    kEarlyLines = 0x00,     // f32* [3] per channel
    kEarlyTaps = 0x0C,      // u32 [3] tap positions, shared
    kEarlyLength = 0x18,    // u32
    kEarlyGains = 0x20,     // f32 [3]
    kPreDelayLines = 0x2C,  // f32* [3] per channel
    kPreDelayPos = 0x38,    // u32
    kPreDelayLength = 0x3C, // u32, 0: no pre-delay
    kCombLines = 0x44,      // f32* [3 channels][3 combs]
    kCombPos = 0x68,        // u32 [3], shared
    kCombLength = 0x74,     // u32 [3]
    kCombGains = 0x8C,      // f32 [3]
    kAllpassLines = 0x98,   // f32* [3 channels][2]
    kAllpassPos = 0xB0,     // u32 [2], shared
    kAllpassLength = 0xB8,  // u32 [2]
    kLastLines = 0xC8,      // f32* [3] per channel: the third allpass
    kLastPos = 0xD4,        // u32 [3]
    kLastLength = 0xE0,     // u32 [3]
    kAllpassGain = 0xF8,    // f32
    kLowpassState = 0xFC,   // f32 [3]
    kLowpassCoef = 0x108,   // f32
    kFlags = 0x10C,         // u32: nonzero skips the frame (and clears bit 1)
    kCrosstalk = 0x12C,     // f32
    kLevel = 0x134,         // f32
    kAuxInput = 0x138,      // s32** [3], or 0
    kSend = 0x13C,          // s32** [3], or 0
    kOutputGain = 0x140,    // f32
    kSendGain = 0x144,      // f32
};

constexpr unsigned kSamples = 96;

uint32_t LoadU32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}
void StoreU32(uint8_t* p, uint32_t v) {
    v = __builtin_bswap32(v);
    std::memcpy(p, &v, 4);
}
double LoadF(const uint8_t* p) {
    const uint32_t bits = LoadU32(p);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
void StoreF(uint8_t* p, double v) { StoreU32(p, MemoryInline::ConvertPpcDoubleToSingleBits(v)); }

// PPC single-precision arithmetic, as the translated code does it. Every
// operand here is a single (loaded as one, or a rounded result), which fmuls's
// 25-bit rounding of the second operand leaves unchanged, so Muls skips it.
double Adds(double a, double b) { return PpcForceSingleValueInline(a + b); }
double Subs(double a, double b) { return PpcForceSingleValueInline(a - b); }
double Muls(double a, double c) { return PpcForceSingleValueInline(a * c); }
uint32_t ToInt(double v) { return PPC_FprLowWordInline(PPC_Fctiwz(v)); }

// A delay line: its samples and the guest RAM behind them.
uint8_t* Line(uint32_t address, uint32_t length) { return Memory::GetPointer(address, size_t(length) * 4); }

} // namespace

static void AudioReverb(CpuContext* ctx) {
    const uint32_t rv = ctx->gpr[4];
    uint8_t* const state = Memory::GetPointer(rv, 0x148);
    if (const uint32_t flags = LoadU32(state + kFlags)) {
        StoreU32(state + kFlags, flags & ~2u);
        return;
    }
    auto u32 = [&](uint32_t offset) { return LoadU32(state + offset); };
    auto f32 = [&](uint32_t offset) { return LoadF(state + offset); };

    uint8_t* in[3];
    for (unsigned c = 0; c < 3; ++c) in[c] = Memory::GetPointer(Memory::Read32(ctx->gpr[3] + 4 * c), kSamples * 4);
    uint8_t* aux[3]{};
    if (const uint32_t a = u32(kAuxInput))
        for (unsigned c = 0; c < 3; ++c) aux[c] = Memory::GetPointer(Memory::Read32(a + 4 * c), kSamples * 4);
    uint8_t* send[3]{};
    if (const uint32_t s = u32(kSend))
        for (unsigned c = 0; c < 3; ++c) send[c] = Memory::GetPointer(Memory::Read32(s + 4 * c), kSamples * 4);

    const uint32_t earlyLength = u32(kEarlyLength), preDelayLength = u32(kPreDelayLength);
    uint8_t *early[3], *preDelay[3]{}, *comb[3][3], *allpass[3][2], *last[3];
    for (unsigned c = 0; c < 3; ++c) {
        early[c] = Line(u32(kEarlyLines + 4 * c), earlyLength);
        if (preDelayLength) preDelay[c] = Line(u32(kPreDelayLines + 4 * c), preDelayLength);
        for (unsigned k = 0; k < 3; ++k) comb[c][k] = Line(u32(kCombLines + 12 * c + 4 * k), u32(kCombLength + 4 * k));
        for (unsigned k = 0; k < 2; ++k)
            allpass[c][k] = Line(u32(kAllpassLines + 8 * c + 4 * k), u32(kAllpassLength + 4 * k));
        last[c] = Line(u32(kLastLines + 4 * c), u32(kLastLength + 4 * c));
    }

    const double coef = f32(kLowpassCoef);
    const double coefRest = Subs(1.0, coef);
    const double wetScale = Muls(0.6000000238418579, f32(kLevel));
    const double cross = Muls(0.5, f32(kCrosstalk));
    const double apGain = f32(kAllpassGain);
    const double outGain = f32(kOutputGain), sendGain = f32(kSendGain);
    double earlyGain[3], combGain[3], lowpassState[3];
    uint32_t taps[3], combPos[3], combLength[3], apPos[2], apLength[2], lastPos[3], lastLength[3];
    for (unsigned k = 0; k < 3; ++k) {
        earlyGain[k] = f32(kEarlyGains + 4 * k);
        combGain[k] = f32(kCombGains + 4 * k);
        lowpassState[k] = f32(kLowpassState + 4 * k);
        taps[k] = u32(kEarlyTaps + 4 * k);
        combPos[k] = u32(kCombPos + 4 * k);
        combLength[k] = u32(kCombLength + 4 * k);
        lastPos[k] = u32(kLastPos + 4 * k);
        lastLength[k] = u32(kLastLength + 4 * k);
    }
    for (unsigned k = 0; k < 2; ++k) {
        apPos[k] = u32(kAllpassPos + 4 * k);
        apLength[k] = u32(kAllpassLength + 4 * k);
    }
    uint32_t preDelayPos = u32(kPreDelayPos);
    auto advance = [](uint32_t& pos, uint32_t length) { pos = pos + 1 < length ? pos + 1 : 0; };

    for (unsigned s = 0; s < kSamples; ++s) {
        double wet[3];
        for (unsigned c = 0; c < 3; ++c) {
            uint32_t sample = LoadU32(in[c] + 4 * s);
            if (aux[c]) sample += LoadU32(aux[c] + 4 * s);
            const double x = PpcForceSingleValueInline(double(int32_t(sample)));

            uint8_t* e = early[c];
            const double e0 = Muls(earlyGain[0], LoadF(e + 4 * taps[0]));
            const double e1 = Muls(earlyGain[1], LoadF(e + 4 * taps[1]));
            const double tap2 = LoadF(e + 4 * taps[2]);
            StoreF(e + 4 * taps[2], x);
            const double e2 = Muls(earlyGain[2], tap2);
            double sum = Adds(e2, Adds(e0, e1));

            double delayed = x;
            if (preDelayLength) {
                uint8_t* p = preDelay[c] + 4 * preDelayPos;
                delayed = LoadF(p);
                StoreF(p, x);
            }
            double combs = 0.0;
            for (unsigned k = 0; k < 3; ++k) {
                uint8_t* p = comb[c][k] + 4 * combPos[k];
                const double y = LoadF(p);
                const double fed = Muls(y, combGain[k]);
                combs = Adds(combs, y);
                StoreF(p, Adds(delayed, fed));
            }
            uint8_t* p = allpass[c][0] + 4 * apPos[0];
            double y = LoadF(p);
            double v = Adds(combs, Muls(y, apGain));
            StoreF(p, v);
            const double ap1 = Subs(y, Muls(v, apGain));
            p = allpass[c][1] + 4 * apPos[1];
            y = LoadF(p);
            v = Adds(ap1, Muls(y, apGain));
            StoreF(p, v);
            const double ap2 = Subs(y, Muls(v, apGain));
            const double lowpass = Adds(Muls(coefRest, ap2), Muls(coef, lowpassState[c]));
            lowpassState[c] = lowpass;

            p = last[c] + 4 * lastPos[c];
            y = LoadF(p);
            v = Adds(lowpass, Muls(y, apGain));
            StoreF(p, v);
            advance(lastPos[c], lastLength[c]);
            const double out = Subs(y, Muls(v, apGain));
            wet[c] = Adds(Muls(out, wetScale), sum);
        }

        const double out0 = Adds(wet[0], Muls(Adds(wet[1], wet[2]), cross));
        const double out1 = Adds(wet[1], Muls(Adds(wet[0], wet[2]), cross));
        const double out2 = Adds(wet[2], Muls(Adds(wet[0], wet[1]), cross));
        StoreU32(in[0] + 4 * s, ToInt(Muls(out0, outGain)));
        StoreU32(in[1] + 4 * s, ToInt(Muls(out1, outGain)));
        StoreU32(in[2] + 4 * s, ToInt(Muls(out2, outGain)));
        if (send[0]) {
            StoreU32(send[0] + 4 * s, ToInt(Muls(out0, sendGain)));
            StoreU32(send[1] + 4 * s, ToInt(Muls(out1, sendGain)));
            StoreU32(send[2] + 4 * s, ToInt(Muls(out2, sendGain)));
        }

        for (unsigned k = 0; k < 3; ++k) advance(taps[k], earlyLength);
        if (preDelayLength) advance(preDelayPos, preDelayLength);
        for (unsigned k = 0; k < 3; ++k) advance(combPos[k], combLength[k]);
        for (unsigned k = 0; k < 2; ++k) advance(apPos[k], apLength[k]);
    }

    // The state the loop kept in registers.
    for (unsigned k = 0; k < 3; ++k) {
        StoreF(state + kLowpassState + 4 * k, lowpassState[k]);
        StoreU32(state + kEarlyTaps + 4 * k, taps[k]);
        StoreU32(state + kCombPos + 4 * k, combPos[k]);
        StoreU32(state + kLastPos + 4 * k, lastPos[k]);
    }
    for (unsigned k = 0; k < 2; ++k) StoreU32(state + kAllpassPos + 4 * k, apPos[k]);
    if (preDelayLength) StoreU32(state + kPreDelayPos, preDelayPos);
}
extern "C" void func_80475880(CpuContext* ctx);

extern "C" void MH3_AudioReverb(CpuContext* ctx) {
    PortCheck::Call(ctx, "AudioReverb", func_80475880, AudioReverb);
}
REGISTER_NATIVE_FUNCTION_AS(0x80475880, MH3_AudioReverb, "AudioReverb");
