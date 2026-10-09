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
// gains and lowpass state in registers for the frame. Every operand is a
// single, and a single-precision add, subtract or multiply rounded once from
// double equals the same operation done in float, so the filter runs in float
// (host FZ follows NI either way) and stores its results' bits as they are.
#include "abi_bridge.h"
#include "memory.h"
#include "port_check.h"
#include "ppc_runtime.h"

#include <cstring>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

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
float LoadF(const uint8_t* p) {
    const uint32_t bits = LoadU32(p);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
void StoreF(uint8_t* p, float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    StoreU32(p, bits);
}

// PPC single-precision arithmetic (fadds, fsubs, fmuls) on singles; separate
// functions, so no two are ever fused.
float Adds(float a, float b) {
    return a + b;
}
float Subs(float a, float b) {
    return a - b;
}
float Muls(float a, float c) {
    return a * c;
}
uint32_t ToInt(float v) {
    return PPC_FprLowWordInline(PPC_Fctiwz(v));
}

// A delay line: its samples and the guest RAM behind them.
uint8_t* Line(uint32_t address, uint32_t length) {
    return Memory::GetPointer(address, size_t(length) * 4);
}

#if defined(__ARM_NEON)
// The three channels as lanes 0-2 of a vector (lane 3 stays zero): each
// access is a gather or scatter of one big-endian word per channel.
float32x4_t Gather(const uint8_t* a0, const uint8_t* a1, const uint8_t* a2) {
    uint32x4_t v = vdupq_n_u32(0);
    v = vld1q_lane_u32(reinterpret_cast<const uint32_t*>(a0), v, 0);
    v = vld1q_lane_u32(reinterpret_cast<const uint32_t*>(a1), v, 1);
    v = vld1q_lane_u32(reinterpret_cast<const uint32_t*>(a2), v, 2);
    return vreinterpretq_f32_u8(vrev32q_u8(vreinterpretq_u8_u32(v)));
}
void Scatter(uint32x4_t v, uint8_t* a0, uint8_t* a1, uint8_t* a2) {
    v = vreinterpretq_u32_u8(vrev32q_u8(vreinterpretq_u8_u32(v)));
    vst1q_lane_u32(reinterpret_cast<uint32_t*>(a0), v, 0);
    vst1q_lane_u32(reinterpret_cast<uint32_t*>(a1), v, 1);
    vst1q_lane_u32(reinterpret_cast<uint32_t*>(a2), v, 2);
}
void Scatter(float32x4_t v, uint8_t* a0, uint8_t* a1, uint8_t* a2) {
    Scatter(vreinterpretq_u32_f32(v), a0, a1, a2);
}
#endif

} // namespace

static void AudioReverb(CpuContext* ctx) {
    const uint32_t rv = ctx->gpr[4];
    uint8_t* const state = Memory::GetPointer(rv, 0x148);
    if (const uint32_t flags = LoadU32(state + kFlags)) {
        StoreU32(state + kFlags, flags & ~2u);
        return;
    }
    auto u32 = [&](uint32_t offset) {
        return LoadU32(state + offset);
    };
    auto f32 = [&](uint32_t offset) {
        return LoadF(state + offset);
    };

    uint8_t* in[3];
    for (unsigned c = 0; c < 3; ++c)
        in[c] = Memory::GetPointer(Memory::Read32(ctx->gpr[3] + 4 * c), kSamples * 4);
    uint8_t* aux[3]{};
    if (const uint32_t a = u32(kAuxInput))
        for (unsigned c = 0; c < 3; ++c)
            aux[c] = Memory::GetPointer(Memory::Read32(a + 4 * c), kSamples * 4);
    uint8_t* send[3]{};
    if (const uint32_t s = u32(kSend))
        for (unsigned c = 0; c < 3; ++c)
            send[c] = Memory::GetPointer(Memory::Read32(s + 4 * c), kSamples * 4);

    const uint32_t earlyLength = u32(kEarlyLength), preDelayLength = u32(kPreDelayLength);
    uint8_t *early[3], *preDelay[3]{}, *comb[3][3], *allpass[3][2], *last[3];
    for (unsigned c = 0; c < 3; ++c) {
        early[c] = Line(u32(kEarlyLines + 4 * c), earlyLength);
        if (preDelayLength)
            preDelay[c] = Line(u32(kPreDelayLines + 4 * c), preDelayLength);
        for (unsigned k = 0; k < 3; ++k)
            comb[c][k] = Line(u32(kCombLines + 12 * c + 4 * k), u32(kCombLength + 4 * k));
        for (unsigned k = 0; k < 2; ++k)
            allpass[c][k] = Line(u32(kAllpassLines + 8 * c + 4 * k), u32(kAllpassLength + 4 * k));
        last[c] = Line(u32(kLastLines + 4 * c), u32(kLastLength + 4 * c));
    }

    const float coef = f32(kLowpassCoef);
    const float coefRest = Subs(1.0f, coef);
    const float wetScale = Muls(0.6f, f32(kLevel));
    const float cross = Muls(0.5f, f32(kCrosstalk));
    const float apGain = f32(kAllpassGain);
    const float outGain = f32(kOutputGain), sendGain = f32(kSendGain);
    float earlyGain[3], combGain[3], lowpassState[3];
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
    auto advance = [](uint32_t& pos, uint32_t length) {
        pos = pos + 1 < length ? pos + 1 : 0;
    };

#if defined(__ARM_NEON)
    // NEON always flushes denormals (as the guest's non-IEEE mode does) and
    // rounds to nearest; with the host FZ bit set the vector lanes compute
    // what the scalar code does. The float to integer conversions saturate
    // like fctiwz (NaN aside, which the filter never makes).
    if ((MkwGetHostFpControl() & kMkwFpControlFlushToZeroBits) != 0) {
        const float32x4_t vCoef = vdupq_n_f32(coef), vCoefRest = vdupq_n_f32(coefRest);
        const float32x4_t vWetScale = vdupq_n_f32(wetScale), vCross = vdupq_n_f32(cross);
        const float32x4_t vApGain = vdupq_n_f32(apGain), vOutGain = vdupq_n_f32(outGain);
        const float32x4_t vSendGain = vdupq_n_f32(sendGain);
        float32x4_t vEarlyGain[3], vCombGain[3];
        for (unsigned k = 0; k < 3; ++k) {
            vEarlyGain[k] = vdupq_n_f32(earlyGain[k]);
            vCombGain[k] = vdupq_n_f32(combGain[k]);
        }
        float32x4_t vLowpass = {lowpassState[0], lowpassState[1], lowpassState[2], 0.0f};
        for (unsigned s = 0; s < kSamples; ++s) {
            const unsigned o = 4 * s;
            // The filter waits on its ~30 delay-line streams (each sample
            // touches every line once); fetch each 16 samples ahead. A prefetch
            // past a line's end is harmless.
            if ((s & 7) == 0) {
                constexpr unsigned ahead = 4 * 16;
                for (unsigned c = 0; c < 3; ++c) {
                    for (unsigned k = 0; k < 3; ++k) {
                        __builtin_prefetch(early[c] + 4 * taps[k] + ahead, 1);
                        __builtin_prefetch(comb[c][k] + 4 * combPos[k] + ahead, 1);
                    }
                    if (preDelayLength)
                        __builtin_prefetch(preDelay[c] + 4 * preDelayPos + ahead, 1);
                    for (unsigned k = 0; k < 2; ++k)
                        __builtin_prefetch(allpass[c][k] + 4 * apPos[k] + ahead, 1);
                    __builtin_prefetch(last[c] + 4 * lastPos[c] + ahead, 1);
                }
            }
            uint32x4_t samples = vreinterpretq_u32_f32(Gather(in[0] + o, in[1] + o, in[2] + o));
            if (aux[0])
                samples = vaddq_u32(samples, vreinterpretq_u32_f32(Gather(aux[0] + o, aux[1] + o, aux[2] + o)));
            const float32x4_t x = vcvtq_f32_s32(vreinterpretq_s32_u32(samples));

            const unsigned t0 = 4 * taps[0], t1 = 4 * taps[1], t2 = 4 * taps[2];
            const float32x4_t e0 = vmulq_f32(vEarlyGain[0], Gather(early[0] + t0, early[1] + t0, early[2] + t0));
            const float32x4_t e1 = vmulq_f32(vEarlyGain[1], Gather(early[0] + t1, early[1] + t1, early[2] + t1));
            const float32x4_t tap2 = Gather(early[0] + t2, early[1] + t2, early[2] + t2);
            Scatter(x, early[0] + t2, early[1] + t2, early[2] + t2);
            const float32x4_t sum = vaddq_f32(vmulq_f32(vEarlyGain[2], tap2), vaddq_f32(e0, e1));

            float32x4_t delayed = x;
            if (preDelayLength) {
                const unsigned pd = 4 * preDelayPos;
                delayed = Gather(preDelay[0] + pd, preDelay[1] + pd, preDelay[2] + pd);
                Scatter(x, preDelay[0] + pd, preDelay[1] + pd, preDelay[2] + pd);
            }
            float32x4_t combs = vdupq_n_f32(0.0f);
            for (unsigned k = 0; k < 3; ++k) {
                const unsigned cp = 4 * combPos[k];
                const float32x4_t y = Gather(comb[0][k] + cp, comb[1][k] + cp, comb[2][k] + cp);
                combs = vaddq_f32(combs, y);
                Scatter(vaddq_f32(delayed, vmulq_f32(y, vCombGain[k])), comb[0][k] + cp, comb[1][k] + cp, comb[2][k] + cp);
            }
            float32x4_t through = combs;
            for (unsigned k = 0; k < 2; ++k) {
                const unsigned ap = 4 * apPos[k];
                const float32x4_t y = Gather(allpass[0][k] + ap, allpass[1][k] + ap, allpass[2][k] + ap);
                const float32x4_t v = vaddq_f32(through, vmulq_f32(y, vApGain));
                Scatter(v, allpass[0][k] + ap, allpass[1][k] + ap, allpass[2][k] + ap);
                through = vsubq_f32(y, vmulq_f32(v, vApGain));
            }
            vLowpass = vaddq_f32(vmulq_f32(vCoefRest, through), vmulq_f32(vCoef, vLowpass));

            uint8_t* const l0 = last[0] + 4 * lastPos[0];
            uint8_t* const l1 = last[1] + 4 * lastPos[1];
            uint8_t* const l2 = last[2] + 4 * lastPos[2];
            const float32x4_t y = Gather(l0, l1, l2);
            const float32x4_t v = vaddq_f32(vLowpass, vmulq_f32(y, vApGain));
            Scatter(v, l0, l1, l2);
            for (unsigned c = 0; c < 3; ++c)
                advance(lastPos[c], lastLength[c]);
            const float32x4_t wet = vaddq_f32(vmulq_f32(vsubq_f32(y, vmulq_f32(v, vApGain)), vWetScale), sum);

            // Each channel gets the other two: (w1 + w2, w0 + w2, w0 + w1).
            const float32x4_t a = {wet[1], wet[0], wet[0], 0.0f}, b = {wet[2], wet[2], wet[1], 0.0f};
            const float32x4_t others = vaddq_f32(a, b);
            const float32x4_t out = vaddq_f32(wet, vmulq_f32(others, vCross));
            Scatter(vreinterpretq_u32_s32(vcvtq_s32_f32(vmulq_f32(out, vOutGain))), in[0] + o, in[1] + o, in[2] + o);
            if (send[0])
                Scatter(vreinterpretq_u32_s32(vcvtq_s32_f32(vmulq_f32(out, vSendGain))), send[0] + o, send[1] + o, send[2] + o);

            for (unsigned k = 0; k < 3; ++k)
                advance(taps[k], earlyLength);
            if (preDelayLength)
                advance(preDelayPos, preDelayLength);
            for (unsigned k = 0; k < 3; ++k)
                advance(combPos[k], combLength[k]);
            for (unsigned k = 0; k < 2; ++k)
                advance(apPos[k], apLength[k]);
        }
        for (unsigned c = 0; c < 3; ++c)
            lowpassState[c] = vLowpass[c];
    } else
#endif
        for (unsigned s = 0; s < kSamples; ++s) {
            float wet[3];
            for (unsigned c = 0; c < 3; ++c) {
                uint32_t sample = LoadU32(in[c] + 4 * s);
                if (aux[c])
                    sample += LoadU32(aux[c] + 4 * s);
                const float x = float(int32_t(sample));

                uint8_t* e = early[c];
                const float e0 = Muls(earlyGain[0], LoadF(e + 4 * taps[0]));
                const float e1 = Muls(earlyGain[1], LoadF(e + 4 * taps[1]));
                const float tap2 = LoadF(e + 4 * taps[2]);
                StoreF(e + 4 * taps[2], x);
                const float e2 = Muls(earlyGain[2], tap2);
                const float sum = Adds(e2, Adds(e0, e1));

                float delayed = x;
                if (preDelayLength) {
                    uint8_t* p = preDelay[c] + 4 * preDelayPos;
                    delayed = LoadF(p);
                    StoreF(p, x);
                }
                float combs = 0.0f;
                for (unsigned k = 0; k < 3; ++k) {
                    uint8_t* p = comb[c][k] + 4 * combPos[k];
                    const float y = LoadF(p);
                    const float fed = Muls(y, combGain[k]);
                    combs = Adds(combs, y);
                    StoreF(p, Adds(delayed, fed));
                }
                uint8_t* p = allpass[c][0] + 4 * apPos[0];
                float y = LoadF(p);
                float v = Adds(combs, Muls(y, apGain));
                StoreF(p, v);
                const float ap1 = Subs(y, Muls(v, apGain));
                p = allpass[c][1] + 4 * apPos[1];
                y = LoadF(p);
                v = Adds(ap1, Muls(y, apGain));
                StoreF(p, v);
                const float ap2 = Subs(y, Muls(v, apGain));
                const float lowpass = Adds(Muls(coefRest, ap2), Muls(coef, lowpassState[c]));
                lowpassState[c] = lowpass;

                p = last[c] + 4 * lastPos[c];
                y = LoadF(p);
                v = Adds(lowpass, Muls(y, apGain));
                StoreF(p, v);
                advance(lastPos[c], lastLength[c]);
                const float out = Subs(y, Muls(v, apGain));
                wet[c] = Adds(Muls(out, wetScale), sum);
            }

            const float out0 = Adds(wet[0], Muls(Adds(wet[1], wet[2]), cross));
            const float out1 = Adds(wet[1], Muls(Adds(wet[0], wet[2]), cross));
            const float out2 = Adds(wet[2], Muls(Adds(wet[0], wet[1]), cross));
            StoreU32(in[0] + 4 * s, ToInt(Muls(out0, outGain)));
            StoreU32(in[1] + 4 * s, ToInt(Muls(out1, outGain)));
            StoreU32(in[2] + 4 * s, ToInt(Muls(out2, outGain)));
            if (send[0]) {
                StoreU32(send[0] + 4 * s, ToInt(Muls(out0, sendGain)));
                StoreU32(send[1] + 4 * s, ToInt(Muls(out1, sendGain)));
                StoreU32(send[2] + 4 * s, ToInt(Muls(out2, sendGain)));
            }

            for (unsigned k = 0; k < 3; ++k)
                advance(taps[k], earlyLength);
            if (preDelayLength)
                advance(preDelayPos, preDelayLength);
            for (unsigned k = 0; k < 3; ++k)
                advance(combPos[k], combLength[k]);
            for (unsigned k = 0; k < 2; ++k)
                advance(apPos[k], apLength[k]);
        }

    // The state the loop kept in registers.
    for (unsigned k = 0; k < 3; ++k) {
        StoreF(state + kLowpassState + 4 * k, lowpassState[k]);
        StoreU32(state + kEarlyTaps + 4 * k, taps[k]);
        StoreU32(state + kCombPos + 4 * k, combPos[k]);
        StoreU32(state + kLastPos + 4 * k, lastPos[k]);
    }
    for (unsigned k = 0; k < 2; ++k)
        StoreU32(state + kAllpassPos + 4 * k, apPos[k]);
    if (preDelayLength)
        StoreU32(state + kPreDelayPos, preDelayPos);
}
extern "C" void func_80475880(CpuContext* ctx);

extern "C" void MH3_AudioReverb(CpuContext* ctx) {
    PortCheck::Call(ctx, "AudioReverb", func_80475880, AudioReverb);
}
REGISTER_NATIVE_FUNCTION_AS(0x80475880, MH3_AudioReverb, "AudioReverb");
