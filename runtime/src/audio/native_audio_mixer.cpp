// Native AX mixer, RMHP08. Runs on the audio thread (Vita core 0): applies the
// game's per-tick voice changes, decodes, resamples, filters and mixes every
// running voice into the main and aux buses, runs the aux reverb and outputs
// 32 kHz stereo.
//
// The per-voice processing follows the AX DSP program's order (as Dolphin's
// AXWii HLE reconstructs it): sample-rate conversion from the accelerator's
// ADPCM/PCM reads, the volume envelope, the one-pole lowpass, the biquad, then
// one ramped volume per enabled bus. Approximations (the port may trade
// bit-exactness for speed): the polyphase SRC uses a 4-tap Catmull-Rom table
// instead of the DSP's coefficient ROM (dsp_coef.bin is no longer needed),
// the aux return happens in the same 3 ms frame instead of two frames later,
// a stopped voice fades over one frame instead of the DSP's depop
// accumulators, the initial time delay and the Wii Remote speaker are not
// mixed, and in surround modes the surround bus is folded into left/right.
#include "native_audio_mixer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace NativeAudio {
namespace {

// The mixer keeps mixing (voices continue with their last parameters) for at
// most this many ticks past the last tick the game produced; after that it
// outputs silence without advancing until the game catches up.
constexpr uint32_t kFreezeTicks = 50;

// Sample-rate ratios are 16.16; 8x bounds one tick's source reads.
constexpr uint32_t kMaxRatio = 8u << 16;
constexpr uint32_t kMaxSource = kTickSamples * 8 + 2;

inline int32_t Clamp16(int32_t v) {
    return v < -0x8000 ? -0x8000 : v > 0x7FFF ? 0x7FFF : v;
}

// 128 phases x 4 taps in Q14, interpolating between the window's second and
// third samples. The DSP's polyphase filter has three coefficient sets
// (AXSetVoiceSrcType 4TAP_8K/12K/16K, coefSelect 0-2); these are Hann-windowed
// sincs with cutoffs at 1/2, 3/4 and 1 of the source Nyquist rate, normalized
// to unity gain per phase. Linear is SRC type 1.
struct SrcTables {
    int32_t polyphase[3][128][4];
    int32_t linear[128][4];
    SrcTables() {
        constexpr double kPi = 3.14159265358979323846;
        constexpr double kCutoff[3] = {0.5, 0.75, 1.0};

        for (int p = 0; p < 128; ++p) {
            const double t = p / 128.0;
            const double x[4] = {-(1 + t), -t, 1 - t, 2 - t};

            for (int set = 0; set < 3; ++set) {
                double h[4], sum = 0;

                for (int k = 0; k < 4; ++k) {
                    const double a = kPi * kCutoff[set] * x[k];
                    const double sinc = std::fabs(a) < 1e-12 ? 1.0 : std::sin(a) / a;
                    const double window = std::fabs(x[k]) < 2 ? 0.5 * (1 + std::cos(kPi * x[k] / 2)) : 0.0;
                    h[k] = sinc * window;
                    sum += h[k];
                }

                for (int k = 0; k < 4; ++k)
                    polyphase[set][p][k] = int32_t(std::lround(h[k] / sum * 16384.0));
            }

            linear[p][0] = 0;
            linear[p][1] = int32_t(std::lround((1.0 - t) * 16384.0));
            linear[p][2] = int32_t(std::lround(t * 16384.0));
            linear[p][3] = 0;
        }
    }
};

const SrcTables kSrc;

// mixerCtrl bus k (two bits each: enable, ramp) -> its PB volume pair.
constexpr uint8_t kVolumeIndex[12] = {0, 1, 8, 2, 3, 9, 4, 5, 10, 6, 7, 11};

uint64_t NowNs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void MixAdd(int32_t* out, const int16_t* in, uint16_t& volume, int16_t delta) {
    uint16_t v = volume;

    if (delta == 0) {
        if (v == 0)
            return;

        for (uint32_t i = 0; i < kTickSamples; ++i)
            out[i] += Clamp16((int32_t(in[i]) * int32_t(v)) >> 15);

        return;
    }

    for (uint32_t i = 0; i < kTickSamples; ++i) {
        out[i] += Clamp16((int32_t(in[i]) * int32_t(v)) >> 15);
        v = uint16_t(v + delta);
    }

    volume = v;
}

// Linear ramp over the frame, as the DSP's OUTPUT/MIX_AUX volume ramps.
void Ramp(uint16_t* out, uint16_t from, uint16_t to) {
    float current = from;
    const float step = (float(to) - float(from)) / float(kTickSamples);

    for (uint32_t i = 0; i < kTickSamples; ++i) {
        current += step;
        out[i] = uint16_t(current);
    }
}

} // namespace

Mixer::Mixer() {
    for (auto& v : voices_) {
        std::memset(v.pb, 0, sizeof(v.pb));
        v.fading = v.published = false;
    }

    for (uint32_t i = 0; i < kMaxVoices; ++i) {
        feedback_.stateVolume[i].store(0, std::memory_order_relaxed);
        feedback_.address[i].store(0, std::memory_order_relaxed);
    }
}

Mixer::~Mixer() = default;

void Mixer::Initialize(const uint8_t* mem1, uint32_t mem1Size, const uint8_t* mem2, uint32_t mem2Size) {
    mem1_ = mem1;
    mem2_ = mem2;
    mem1Size_ = mem1Size;
    mem2Size_ = mem2Size;
    ring_.Clear();
    ResetVoices();
    rendered_ = 0;
    tickPosition_ = kTickSamples;
    produced_.store(0, std::memory_order_relaxed);
    feedback_.appliedTick.store(0, std::memory_order_relaxed);
    feedback_.renderedTicks.store(0, std::memory_order_release);
}

void Mixer::ResetVoices() {
    for (uint32_t i = 0; i < kMaxVoices; ++i) {
        std::memset(voices_[i].pb, 0, sizeof(voices_[i].pb));
        voices_[i].fading = false;
        voices_[i].published = false;
        feedback_.stateVolume[i].store(0, std::memory_order_relaxed);
        feedback_.address[i].store(0, std::memory_order_relaxed);
    }

    for (auto& aux : aux_)
        aux.reset();

    masterVolume_ = lastMasterVolume_ = 0x8000;

    for (uint32_t a = 0; a < 3; ++a)
        auxReturn_[a] = lastAuxReturn_[a] = 0x8000;

    mode_ = 0;
}

// DSP accelerator addresses are physical: bit 28 selects MEM2 (64 MiB),
// otherwise MEM1 (24 MiB). Anything else reads as silence.
inline uint8_t Mixer::ByteAt(uint32_t address) const {
    if (address & 0x10000000u) {
        const uint32_t offset = address & 0x03FFFFFFu;
        return offset < mem2Size_ ? mem2_[offset] : 0;
    }

    const uint32_t offset = address & 0x01FFFFFFu;
    return offset < mem1Size_ ? mem1_[offset] : 0;
}

void Mixer::Render(int16_t* out, uint32_t frames) {
    const uint64_t start = NowNs();
    const uint32_t total = frames;

    while (frames) {
        if (tickPosition_ == kTickSamples) {
            RenderTick(tick_);
            tickPosition_ = 0;
        }

        const uint32_t n = std::min(frames, kTickSamples - tickPosition_);
        std::memcpy(out, tick_ + tickPosition_ * 2, n * 2 * sizeof(int16_t));
        out += n * 2;
        frames -= n;
        tickPosition_ += n;
    }

    statistics_.renderNs.fetch_add(NowNs() - start, std::memory_order_relaxed);
    statistics_.outputFrames.fetch_add(total, std::memory_order_relaxed);
}

uint32_t Mixer::ApplyCommands(uint32_t tick) {
    uint32_t applied = 0;
    bool any = false;

    while (const uint32_t* record = ring_.Peek()) {
        const uint32_t header = record[0];

        switch (HeaderType(header)) {
        case Command::Tick:
            if (int32_t(record[1] - tick) > 0)
                goto done;

            applied = record[1] + 1;
            any = true;
            break;
        case Command::Voice:
            ApplyVoice(record);
            break;
        case Command::Stop: {
            Voice& v = voices_[HeaderVoice(header)];

            if (v.pb[Pb::State] == 1) {
                v.fading = (HeaderFlags(header) & kFlagDepop) != 0;
                v.published = true;
            }

            v.pb[Pb::State] = 0;
            break;
        }
        case Command::Globals:
            masterVolume_ = uint16_t(record[1]);
            mode_ = uint16_t(record[1] >> 16);
            auxReturn_[0] = uint16_t(record[2]);
            auxReturn_[1] = uint16_t(record[2] >> 16);
            auxReturn_[2] = uint16_t(record[3]);
            break;
        case Command::Aux:
            ApplyAux(HeaderVoice(header), record);
            break;
        case Command::Reset:
            ResetVoices();
            break;
        case Command::Pad:
            break;
        }

        ring_.Pop(HeaderWords(header));
    }
done:
    return any ? applied : 0;
}

void Mixer::ApplyVoice(const uint32_t* record) {
    const uint32_t header = record[0];
    Voice& v = voices_[HeaderVoice(header) % kMaxVoices];
    const bool wasRunning = v.pb[Pb::State] == 1;
    uint32_t groups = record[1];
    const uint32_t* data = record + 2;
    uint32_t half = 0;

    while (groups) {
        const uint32_t bit = uint32_t(__builtin_ctz(groups));
        groups &= groups - 1;
        const SyncGroup g = kSyncGroups[bit];

        for (uint32_t i = 0; i < g.count; ++i, ++half)
            v.pb[g.offset + i] = uint16_t(data[half >> 1] >> ((half & 1) * 16));
    }

    if (v.pb[Pb::State] == 1)
        v.fading = false;
    else if (wasRunning)
        v.fading = (HeaderFlags(header) & kFlagDepop) != 0;

    v.published = true;
}

void Mixer::ApplyAux(uint32_t bus, const uint32_t* record) {
    if (bus >= 3)
        return;

    if (AuxKind(record[1]) == AuxKind::ReverbHi) {
        ReverbConfig config;
        std::memcpy(&config, record + 2, sizeof(config));
        aux_[bus] = std::make_unique<Reverb>(config);
    } else {
        aux_[bus].reset();
    }
}

uint32_t Mixer::Decode(Voice& voice, int16_t* out, uint32_t count) {
    uint16_t* pb = voice.pb;
    const uint16_t format = pb[Pb::Format];
    const uint32_t start = (uint32_t(pb[Pb::LoopHi]) << 16 | pb[Pb::LoopLo]) & 0x3FFFFFFFu;
    const uint32_t end = (uint32_t(pb[Pb::EndHi]) << 16 | pb[Pb::EndLo]) & 0x3FFFFFFFu;
    uint32_t cur = (uint32_t(pb[Pb::CurHi]) << 16 | pb[Pb::CurLo]) & 0xBFFFFFFFu;
    const bool loop = pb[Pb::LoopFlag] != 0;
    bool stopped = false;
    uint32_t i = 0;

    if ((format & 3) == 0) {
        // ADPCM: nibble addresses; a header byte (predictor, scale) starts
        // every 16 nibbles.
        uint32_t ps = pb[Pb::AdpcmPredScale] & 0x7F;
        int32_t yn1 = int16_t(pb[Pb::AdpcmYn1]), yn2 = int16_t(pb[Pb::AdpcmYn2]);
        const bool stream = pb[Pb::Type] == 1;

        while (i < count) {
            const uint8_t byte = ByteAt(cur >> 1);
            int32_t nibble = (cur & 1) ? (byte & 0xF) : (byte >> 4);

            if (nibble >= 8)
                nibble -= 16;

            const uint32_t ci = (ps >> 4) & 7;
            const int32_t c1 = int16_t(pb[Pb::AdpcmCoefs + ci * 2]), c2 = int16_t(pb[Pb::AdpcmCoefs + ci * 2 + 1]);
            int32_t value = nibble * (1 << (ps & 0xF)) + ((0x400 + c1 * yn1 + c2 * yn2) >> 11);
            value = value < -0x7FFF ? -0x7FFF : value > 0x7FFF ? 0x7FFF : value;
            yn2 = yn1;
            yn1 = value;
            out[i++] = int16_t(value);
            ++cur;
            uint32_t step = 2;

            if ((end & 0xF) == 0 && cur == end) {
                cur = start + 1;
            } else if ((end & 0xF) == 1 && cur == end - 1) {
                cur = start;
            } else if ((cur & 0xF) == 0) {
                ps = ByteAt(cur >> 1) & 0x7F;
                cur += 2;
                step += 2;
            }

            if (cur == end + step - 1) {
                cur = start;

                if (!loop) {
                    stopped = true;
                    break;
                }

                ps = pb[Pb::LoopPredScale] & 0x7F;

                if (!stream) {
                    yn1 = int16_t(pb[Pb::LoopYn1]);
                    yn2 = int16_t(pb[Pb::LoopYn2]);
                }
            }

            cur &= 0xBFFFFFFFu;
        }

        pb[Pb::AdpcmPredScale] = uint16_t(ps);
        pb[Pb::AdpcmYn1] = uint16_t(yn1);
        pb[Pb::AdpcmYn2] = uint16_t(yn2);
    } else {
        // PCM16 (0x0A, word addresses, gain >> 11) or PCM8 (0x19, byte
        // addresses, gain >> 0 wrapping to 16 bits).
        const bool pcm16 = (format & 3) == 2;
        const uint32_t gainScale = (format >> 4) & 3;
        const uint32_t shift = gainScale == 1 ? 0 : gainScale == 2 ? 16 : 11;
        const int32_t gain = int16_t(pb[Pb::AdpcmGain]);

        while (i < count) {
            int32_t raw;

            if (pcm16) {
                const uint32_t b = cur * 2;
                raw = int16_t(uint16_t(ByteAt(b) << 8 | ByteAt(b + 1)));
            } else {
                raw = ByteAt(cur);
            }

            out[i++] = int16_t((gain * raw) >> shift);
            ++cur;

            if (cur == end + 1) {
                cur = start;

                if (!loop) {
                    stopped = true;
                    break;
                }
            }

            cur &= 0xBFFFFFFFu;
        }
    }

    pb[Pb::CurHi] = uint16_t(cur >> 16);
    pb[Pb::CurLo] = uint16_t(cur);

    if (stopped) {
        pb[Pb::State] = 0;
        std::fill(out + i, out + count, int16_t(0));
    }

    return i;
}

void Mixer::ProcessVoice(Voice& voice, bool fade) {
    uint16_t* pb = voice.pb;
    uint32_t ratio = uint32_t(pb[Pb::SrcRatioHi]) << 16 | pb[Pb::SrcRatioLo];

    if (ratio > kMaxRatio)
        ratio = kMaxRatio;

    const uint32_t srcType = pb[Pb::SrcSelect];
    alignas(16) int16_t source[4 + kMaxSource];

    for (uint32_t k = 0; k < 4; ++k)
        source[k] = int16_t(pb[Pb::SrcLast + k]);

    uint32_t frac = pb[Pb::SrcFrac];
    alignas(16) int16_t samples[kTickSamples];

    if (srcType >= 2) {
        Decode(voice, samples, kTickSamples);

        for (uint32_t k = 0; k < 4; ++k)
            pb[Pb::SrcLast + k] = uint16_t(samples[kTickSamples - 4 + k]);
    } else {
        const uint32_t need = uint32_t((uint64_t(frac) + uint64_t(ratio) * kTickSamples) >> 16);
        Decode(voice, source + 4, need);
        const auto& table = srcType == 0 ? kSrc.polyphase[std::min<uint32_t>(pb[Pb::CoefSelect], 2)] : kSrc.linear;
        uint32_t consumed = 0;

        for (uint32_t i = 0; i < kTickSamples; ++i) {
            frac += ratio;
            consumed += frac >> 16;
            frac &= 0xFFFF;
            const int16_t* w = source + consumed;
            const int32_t* c = table[frac >> 9];
            samples[i] = int16_t(Clamp16((w[0] * c[0] + w[1] * c[1] + w[2] * c[2] + w[3] * c[3] + 0x2000) >> 14));
        }

        pb[Pb::SrcFrac] = uint16_t(frac);

        for (uint32_t k = 0; k < 4; ++k)
            pb[Pb::SrcLast + k] = uint16_t(source[consumed + k]);
    }

    // Volume envelope: a 16-bit volume (0x8000 = unity) stepped per sample.
    uint16_t ve = pb[Pb::VeVolume];
    const int16_t veDelta = int16_t(pb[Pb::VeDelta]);

    if (veDelta == 0) {
        if (ve != 0x8000)
            for (uint32_t i = 0; i < kTickSamples; ++i)
                samples[i] = int16_t(Clamp16((int32_t(samples[i]) * int32_t(ve)) >> 15));
    } else {
        for (uint32_t i = 0; i < kTickSamples; ++i) {
            samples[i] = int16_t(Clamp16((int32_t(samples[i]) * int32_t(ve)) >> 15));
            ve = uint16_t(ve + veDelta);
        }

        pb[Pb::VeVolume] = ve;
    }

    if (pb[Pb::LpfOn]) {
        const int32_t a0 = pb[Pb::LpfA0], b0 = pb[Pb::LpfB0];
        int32_t y = int16_t(pb[Pb::LpfYn1]);

        for (uint32_t i = 0; i < kTickSamples; ++i) {
            y = Clamp16((a0 * samples[i] + b0 * y) >> 15);
            samples[i] = int16_t(y);
        }

        pb[Pb::LpfYn1] = uint16_t(y);
    }

    if (pb[Pb::BiquadOn]) {
        const int32_t b0 = int16_t(pb[Pb::BiquadB0]), b1 = int16_t(pb[Pb::BiquadB1]), b2 = int16_t(pb[Pb::BiquadB2]);
        const int32_t a1 = int16_t(pb[Pb::BiquadA1]), a2 = int16_t(pb[Pb::BiquadA2]);
        int32_t xn1 = int16_t(pb[Pb::BiquadXn1]), xn2 = int16_t(pb[Pb::BiquadXn2]);
        int32_t yn1 = int16_t(pb[Pb::BiquadYn1]), yn2 = int16_t(pb[Pb::BiquadYn2]);

        for (uint32_t i = 0; i < kTickSamples; ++i) {
            const int32_t x = samples[i];
            int64_t acc = int64_t(b0) * x + int64_t(b1) * xn1 + int64_t(b2) * xn2 + int64_t(a1) * yn1 + int64_t(a2) * yn2;
            acc <<= 2;
            acc += (acc & 0x10000) ? 0x8000 : 0x7FFF;
            const int32_t y = Clamp16(int32_t(acc >> 16));
            xn2 = xn1;
            xn1 = x;
            yn2 = yn1;
            yn1 = y;
            samples[i] = int16_t(y);
        }

        pb[Pb::BiquadXn1] = uint16_t(xn1);
        pb[Pb::BiquadXn2] = uint16_t(xn2);
        pb[Pb::BiquadYn1] = uint16_t(yn1);
        pb[Pb::BiquadYn2] = uint16_t(yn2);
    }

    if (fade)
        for (uint32_t i = 0; i < kTickSamples; ++i)
            samples[i] = int16_t(int32_t(samples[i]) * int32_t(kTickSamples - 1 - i) / int32_t(kTickSamples));

    // mixerCtrl (this SDK's layout, as MIXUpdateSettings builds it): per group
    // of main/aux A/B/C, left, right, a left+right ramp, surround and a
    // surround ramp; a ramp bit also enables its buses.
    const uint32_t control = uint32_t(pb[Pb::MixerCtrlHi]) << 16 | pb[Pb::MixerCtrlLo];
    uint32_t enabled = 0, ramped = 0;
    static constexpr uint32_t kGroupShift[4] = {0, 16, 21, 26};

    for (uint32_t group = 0; group < 4; ++group) {
        const uint32_t bits = control >> kGroupShift[group];
        const uint32_t lr = (bits & 4) ? 3u : 0u, s = (bits & 0x10) ? 4u : 0u;
        enabled |= ((bits & 3) | lr | ((bits & 8) >> 1) | s) << (3 * group);
        ramped |= (lr | s) << (3 * group);
    }

    if (mode_ == 0)
        enabled &= ~4u;  // stereo: the main surround bus is not heard
    for (uint32_t a = 0; a < 3; ++a)
        if (!aux_[a])
            enabled &= ~(7u << (3 + 3 * a));  // no effect: the aux send is never returned
    while (enabled) {
        const uint32_t b = uint32_t(__builtin_ctz(enabled));
        enabled &= enabled - 1;
        const uint32_t pair = Pb::Mix + 2u * kVolumeIndex[b];
        const int16_t delta = (ramped & (1u << b)) ? int16_t(pb[pair + 1]) : int16_t(0);
        MixAdd(bus_[b], samples, pb[pair], delta);
    }
}

void Mixer::RenderTick(int16_t* out) {
    const uint32_t produced = produced_.load(std::memory_order_acquire);
    const int32_t ahead = int32_t(rendered_ - produced);

    if (ahead >= int32_t(kFreezeTicks)) {
        std::memset(out, 0, kTickSamples * 2 * sizeof(int16_t));
        statistics_.frozenTicks.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (ahead >= 0)
        statistics_.lateTicks.fetch_add(1, std::memory_order_relaxed);

    const uint32_t applied = ApplyCommands(rendered_);

    std::memset(bus_, 0, sizeof(bus_));
    uint32_t active = 0;

    for (uint32_t i = 0; i < kMaxVoices; ++i) {
        Voice& v = voices_[i];
        const bool running = v.pb[Pb::State] == 1;

        if (running || v.fading) {
            ++active;
            ProcessVoice(v, !running);
            v.fading = false;
            v.published = true;
        }

        if (v.published) {
            feedback_.stateVolume[i].store(uint32_t(v.pb[Pb::State]) << 16 | v.pb[Pb::VeVolume], std::memory_order_relaxed);
            feedback_.address[i].store(uint32_t(v.pb[Pb::CurHi]) << 16 | v.pb[Pb::CurLo], std::memory_order_relaxed);
            v.published = v.pb[Pb::State] == 1;
        }
    }

    // Aux effects, then their returns into the main buses.
    for (uint32_t a = 0; a < 3; ++a) {
        if (!aux_[a])
            continue;

        int32_t* l = bus_[3 + 3 * a];
        int32_t* r = bus_[4 + 3 * a];
        int32_t* s = bus_[5 + 3 * a];
        aux_[a]->Process(l, r, s);
        uint16_t ramp[kTickSamples];
        Ramp(ramp, lastAuxReturn_[a], auxReturn_[a]);
        lastAuxReturn_[a] = auxReturn_[a];

        for (uint32_t i = 0; i < kTickSamples; ++i) {
            bus_[0][i] += int32_t((int64_t(l[i]) * ramp[i]) >> 15);
            bus_[1][i] += int32_t((int64_t(r[i]) * ramp[i]) >> 15);
            bus_[2][i] += int32_t((int64_t(s[i]) * ramp[i]) >> 15);
        }
    }

    uint16_t master[kTickSamples];
    Ramp(master, lastMasterVolume_, masterVolume_);
    lastMasterVolume_ = masterVolume_;
    const bool fold = mode_ != 0;

    for (uint32_t i = 0; i < kTickSamples; ++i) {
        int64_t l = bus_[0][i], r = bus_[1][i];

        if (fold) {
            l += bus_[2][i] / 2;
            r += bus_[2][i] / 2;
        }

        out[i * 2] = int16_t(Clamp16(int32_t(std::clamp<int64_t>((l * master[i]) >> 15, -0x8000, 0x7FFF))));
        out[i * 2 + 1] = int16_t(Clamp16(int32_t(std::clamp<int64_t>((r * master[i]) >> 15, -0x8000, 0x7FFF))));
    }

    // The applied tick is published after the voices' feedback: a game-side
    // field waiting for this tick then reads at least this tick's values.
    if (applied)
        feedback_.appliedTick.store(applied, std::memory_order_release);

    ++rendered_;
    feedback_.renderedTicks.store(rendered_, std::memory_order_release);
    statistics_.renderedTicks.fetch_add(1, std::memory_order_relaxed);
    statistics_.activeVoices.store(active, std::memory_order_relaxed);

    if (active > statistics_.peakVoices.load(std::memory_order_relaxed))
        statistics_.peakVoices.store(active, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// ReverbHiExp on native delay lines. The structure and the gains are the
// guest's (AXFX ReverbHi, see audio_reverb.cpp for the reconstruction): three
// early-reflection taps, an optional pre-delay, three combs, two allpasses, a
// one-pole lowpass and a third allpass per channel, then crosstalk between the
// three channels. Floats, not the PPC's single-rounding sequence.
// ---------------------------------------------------------------------------
Reverb::Reverb(const ReverbConfig& config) : config_(config) {
    for (uint32_t c = 0; c < 3; ++c) {
        early_[c].Allocate(config.earlyLength);

        if (config.preDelayLength)
            preDelay_[c].Allocate(config.preDelayLength);

        for (uint32_t k = 0; k < 3; ++k)
            comb_[c][k].Allocate(config.combLength[k]);

        for (uint32_t k = 0; k < 2; ++k)
            allpass_[c][k].Allocate(config.allpassLength[k]);

        last_[c].Allocate(config.lastLength[c]);
    }

    auto clampPosition = [](uint32_t& position, uint32_t length) {
        if (position >= length)
            position = 0;
    };

    for (uint32_t k = 0; k < 3; ++k) {
        clampPosition(config_.earlyTaps[k], config_.earlyLength);
        clampPosition(config_.combPos[k], config_.combLength[k]);
        clampPosition(config_.lastPos[k], config_.lastLength[k]);
    }

    for (uint32_t k = 0; k < 2; ++k)
        clampPosition(config_.allpassPos[k], config_.allpassLength[k]);

    clampPosition(config_.preDelayPos, config_.preDelayLength);
}

void Reverb::Process(int32_t* left, int32_t* right, int32_t* surround) {
    int32_t* io[3] = {left, right, surround};
    ReverbConfig& s = config_;
    const float coef = s.lowpassCoef, coefRest = 1.0f - coef;
    const float wetScale = 0.6f * s.level, cross = 0.5f * s.crosstalk;
    const float g = s.allpassGain;
    auto advance = [](uint32_t& position, uint32_t length) { position = position + 1 < length ? position + 1 : 0; };

    for (uint32_t i = 0; i < kTickSamples; ++i) {
        float wet[3];

        for (uint32_t c = 0; c < 3; ++c) {
            const float x = float(io[c][i]);
            float* e = early_[c].samples.get();
            const float tap2 = e[s.earlyTaps[2]];
            const float sum = s.earlyGains[0] * e[s.earlyTaps[0]] + s.earlyGains[1] * e[s.earlyTaps[1]] + s.earlyGains[2] * tap2;
            e[s.earlyTaps[2]] = x;
            float delayed = x;

            if (s.preDelayLength) {
                float* p = preDelay_[c].samples.get() + s.preDelayPos;
                delayed = *p;
                *p = x;
            }

            float combs = 0.0f;

            for (uint32_t k = 0; k < 3; ++k) {
                float* p = comb_[c][k].samples.get() + s.combPos[k];
                const float y = *p;
                combs += y;
                *p = delayed + y * s.combGains[k];
            }

            float through = combs;

            for (uint32_t k = 0; k < 2; ++k) {
                float* p = allpass_[c][k].samples.get() + s.allpassPos[k];
                const float y = *p;
                const float v = through + y * g;
                *p = v;
                through = y - v * g;
            }

            lowpass_[c] = coefRest * through + coef * lowpass_[c];
            float* p = last_[c].samples.get() + s.lastPos[c];
            const float y = *p;
            const float v = lowpass_[c] + y * g;
            *p = v;
            advance(s.lastPos[c], s.lastLength[c]);
            wet[c] = (y - v * g) * wetScale + sum;
        }

        const float out0 = wet[0] + (wet[1] + wet[2]) * cross;
        const float out1 = wet[1] + (wet[0] + wet[2]) * cross;
        const float out2 = wet[2] + (wet[0] + wet[1]) * cross;
        auto toInt = [](float v) { return v >= 2147483520.0f ? INT32_MAX : v <= -2147483648.0f ? INT32_MIN : int32_t(v); };
        left[i] = toInt(out0 * s.outGain);
        right[i] = toInt(out1 * s.outGain);
        surround[i] = toInt(out2 * s.outGain);

        for (uint32_t k = 0; k < 3; ++k)
            advance(s.earlyTaps[k], s.earlyLength);

        if (s.preDelayLength)
            advance(s.preDelayPos, s.preDelayLength);

        for (uint32_t k = 0; k < 3; ++k)
            advance(s.combPos[k], s.combLength[k]);

        for (uint32_t k = 0; k < 2; ++k)
            advance(s.allpassPos[k], s.allpassLength[k]);
    }
}

Mixer& GetMixer() {
    static Mixer* mixer = new Mixer;  // lives until exit; the audio thread may still render
    return *mixer;
}

void MixerRender(int16_t* out, uint32_t frames) {
    GetMixer().Render(out, frames);
}

} // namespace NativeAudio
