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
#include <cstring>
#include <utility>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

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
    alignas(8) int16_t polyphase[3][128][4];  // Q14, at most 16384
    alignas(8) int16_t linear[128][4];
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
                    polyphase[set][p][k] = int16_t(std::lround(h[k] / sum * 16384.0));
            }

            linear[p][0] = 0;
            linear[p][1] = int16_t(std::lround((1.0 - t) * 16384.0));
            linear[p][2] = int16_t(std::lround(t * 16384.0));
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

#if defined(__ARM_NEON)
// Four samples' Clamp16((x * v) >> 15): the saturating narrow is the clamp.
inline int16x4_t ScaleClamp(int16x4_t x, uint32x4_t v) {
    return vqshrn_n_s32(vmulq_s32(vmovl_s16(x), vreinterpretq_s32_u32(v)), 15);
}

// The volumes of four samples from `volume` on, stepping by `delta` with the
// 16-bit wrap, and the step to the next four.
inline uint32x4_t Volumes(uint16_t volume, int16_t delta) {
    const uint32_t lanes[4] = {volume, uint32_t(volume + delta) & 0xFFFF, uint32_t(volume + 2 * delta) & 0xFFFF, uint32_t(volume + 3 * delta) & 0xFFFF};
    return vld1q_u32(lanes);
}

inline uint32x4_t NextVolumes(uint32x4_t v, int16_t delta) {
    return vandq_u32(vaddq_u32(v, vdupq_n_u32(uint32_t(4 * int32_t(delta)))), vdupq_n_u32(0xFFFF));
}
#endif

// samples[i] = Clamp16((samples[i] * v) >> 15), v stepping by `delta`; returns
// the volume after the tick.
uint16_t ScaleSamples(int16_t* samples, uint16_t volume, int16_t delta) {
#if defined(__ARM_NEON)
    uint32x4_t v = Volumes(volume, delta);

    for (uint32_t i = 0; i < kTickSamples; i += 4) {
        vst1_s16(samples + i, ScaleClamp(vld1_s16(samples + i), v));
        v = NextVolumes(v, delta);
    }

    return uint16_t(volume + int32_t(delta) * int32_t(kTickSamples));
#else
    for (uint32_t i = 0; i < kTickSamples; ++i) {
        samples[i] = int16_t(Clamp16((int32_t(samples[i]) * int32_t(volume)) >> 15));
        volume = uint16_t(volume + delta);
    }

    return volume;
#endif
}

void MixAdd(int32_t* out, const int16_t* in, uint16_t& volume, int16_t delta) {
    if (delta == 0 && volume == 0)
        return;

#if defined(__ARM_NEON)
    uint32x4_t v = Volumes(volume, delta);

    for (uint32_t i = 0; i < kTickSamples; i += 4) {
        vst1q_s32(out + i, vaddw_s16(vld1q_s32(out + i), ScaleClamp(vld1_s16(in + i), v)));
        v = NextVolumes(v, delta);
    }

    volume = uint16_t(volume + int32_t(delta) * int32_t(kTickSamples));
#else
    uint16_t v = volume;

    for (uint32_t i = 0; i < kTickSamples; ++i) {
        out[i] += Clamp16((int32_t(in[i]) * int32_t(v)) >> 15);
        v = uint16_t(v + delta);
    }

    volume = v;
#endif
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
        v.pcm = nullptr;
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
    decoded_.clear();
    rewritten_.clear();
    decodedBytes_ = 0;
    rendered_ = 0;
    tickPosition_ = kTickSamples;
    produced_.store(0, std::memory_order_relaxed);
    feedback_.appliedTick.store(0, std::memory_order_relaxed);
    feedback_.renderedTicks.store(0, std::memory_order_release);
}

void Mixer::ResetVoices() {
    for (uint32_t i = 0; i < kMaxVoices; ++i) {
        std::memset(voices_[i].pb, 0, sizeof(voices_[i].pb));
        voices_[i].pcm = nullptr;
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

    // New addresses or coefficients: the voice finds its decoded copy again,
    // which checks the sample's bytes.
    if (groups & (Guest::kSyncAddr | Guest::kSyncLoopAddr | Guest::kSyncEndAddr | Guest::kSyncCurrAddr | Guest::kSyncAdpcm | Guest::kSyncType | Guest::kSyncAllParams))
        v.pcm = nullptr;

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

    if (AuxKind(record[1]) == AuxKind::ReverbGains) {
        if (aux_[bus]) {
            float gains[4];
            std::memcpy(gains, record + 2, sizeof(gains));
            aux_[bus]->SetGains(gains);
        }
    } else if (AuxKind(record[1]) == AuxKind::ReverbHi) {
        ReverbConfig config;
        std::memcpy(&config, record + 2, sizeof(config));
        aux_[bus] = std::make_unique<Reverb>(config);
    } else {
        aux_[bus].reset();
    }
}

namespace {

// Decoded copies take at most this much; the least recently played go first.
constexpr size_t kDecodedBudget = size_t(16) << 20;

// Every 61st byte (all offsets in the 8-byte frames over a sample), the last
// byte and the coefficients: a bank loaded over the sample changes it.
uint32_t SampleCheck(const uint8_t* bytes, uint32_t size, const uint16_t* coefs) {
    uint32_t hash = 2166136261u ^ size;

    for (uint32_t i = 0; i < size; i += 61)
        hash = (hash ^ bytes[i]) * 16777619u;

    hash = (hash ^ bytes[size - 1]) * 16777619u;

    for (uint32_t k = 0; k < 16; ++k)
        hash = (hash ^ coefs[k]) * 16777619u;

    return hash;
}

uint32_t SampleBytes(const PcmSample& sample) {
    return (sample.end >> 1) - (sample.base >> 1) + 1;
}

// Decodes whole frames until the first `samples` are decoded, as
// DecodeFromMemory does sample by sample.
void DecodeFrames(PcmSample& sample, uint32_t samples) {
    const uint32_t total = uint32_t(sample.pcm.size());
    int32_t yn1 = sample.yn1, yn2 = sample.yn2;

    while (sample.frames * 14 < samples) {
        const uint8_t* frame = sample.data + sample.frames * 8;
        const uint32_t ps = frame[0] & 0x7F;
        const uint32_t ci = (ps >> 4) & 7;
        const int32_t c1 = int16_t(sample.coefs[ci * 2]), c2 = int16_t(sample.coefs[ci * 2 + 1]);
        const int32_t scale = 1 << (ps & 0xF);
        const uint32_t first = sample.frames * 14, n = std::min(14u, total - first);
        int16_t* out = sample.pcm.data() + first;

        for (uint32_t k = 0; k < n; ++k) {
            const uint8_t byte = frame[1 + k / 2];
            int32_t nibble = (k & 1) ? (byte & 0xF) : (byte >> 4);

            if (nibble >= 8)
                nibble -= 16;

            int32_t value = nibble * scale + ((0x400 + c1 * yn1 + c2 * yn2) >> 11);
            value = value < -0x7FFF ? -0x7FFF : value > 0x7FFF ? 0x7FFF : value;
            yn2 = yn1;
            yn1 = value;
            out[k] = int16_t(value);
        }

        ++sample.frames;
    }

    sample.yn1 = yn1;
    sample.yn2 = yn2;
}

} // namespace

const uint8_t* Mixer::BytesAt(uint32_t address, uint32_t bytes) const {
    if (address & 0x10000000u) {
        const uint32_t offset = address & 0x03FFFFFFu;
        return uint64_t(offset) + bytes <= mem2Size_ ? mem2_ + offset : nullptr;
    }

    const uint32_t offset = address & 0x01FFFFFFu;
    return uint64_t(offset) + bytes <= mem1Size_ ? mem1_ + offset : nullptr;
}

void Mixer::ForgetDecoded(uint32_t end) {
    const auto found = decoded_.find(end);

    if (found == decoded_.end())
        return;

    for (Voice& v : voices_)
        if (v.pcm == found->second.get())
            v.pcm = nullptr;

    decodedBytes_ -= found->second->pcm.size() * sizeof(int16_t);
    decoded_.erase(found);
    statistics_.decodedKiB.store(uint32_t(decodedBytes_ >> 10), std::memory_order_relaxed);
}

// The decoded copy of the bank sample a non-stream ADPCM voice plays, made
// when none matches. Not for a voice starting inside a frame header, before
// its loop's frames, or on data outside guest memory.
PcmSample* Mixer::FindDecoded(const Voice& voice) {
    const uint16_t* pb = voice.pb;
    const uint32_t start = (uint32_t(pb[Pb::LoopHi]) << 16 | pb[Pb::LoopLo]) & 0x3FFFFFFFu;
    const uint32_t end = (uint32_t(pb[Pb::EndHi]) << 16 | pb[Pb::EndLo]) & 0x3FFFFFFFu;
    const uint32_t cur = (uint32_t(pb[Pb::CurHi]) << 16 | pb[Pb::CurLo]) & 0xBFFFFFFFu;
    const bool loop = pb[Pb::LoopFlag] != 0;

    if ((cur & 0xF) < 2 || (end & 0xF) < 2 || cur > end || (loop && ((start & 0xF) < 2 || start < cur || start > end)))
        return nullptr;

    if (rewritten_.count(end))
        return nullptr;

    if (const auto found = decoded_.find(end); found != decoded_.end()) {
        PcmSample& sample = *found->second;
        const uint32_t bytes = SampleBytes(sample);

        if (sample.base <= cur && BytesAt(sample.base >> 1, bytes) == sample.data && SampleCheck(sample.data, bytes, pb + Pb::AdpcmCoefs) == sample.check)
            return &sample;

        ForgetDecoded(end);
    }

    auto sample = std::make_unique<PcmSample>();
    sample->base = cur & ~0xFu;
    sample->end = end;
    const uint32_t bytes = SampleBytes(*sample);
    sample->data = BytesAt(sample->base >> 1, bytes);

    if (!sample->data)
        return nullptr;

    std::memcpy(sample->coefs, pb + Pb::AdpcmCoefs, sizeof(sample->coefs));
    sample->check = SampleCheck(sample->data, bytes, sample->coefs);
    sample->yn1 = int16_t(pb[Pb::AdpcmYn1]);
    sample->yn2 = int16_t(pb[Pb::AdpcmYn2]);
    sample->pcm.resize(sample->Index(end) + 1);
    sample->lastUse = rendered_;
    decodedBytes_ += sample->pcm.size() * sizeof(int16_t);

    while (decodedBytes_ > kDecodedBudget && !decoded_.empty()) {
        auto oldest = decoded_.begin();

        for (auto it = decoded_.begin(); it != decoded_.end(); ++it)
            if (it->second->lastUse < oldest->second->lastUse)
                oldest = it;

        ForgetDecoded(oldest->first);
    }

    PcmSample* made = sample.get();
    decoded_[end] = std::move(sample);
    statistics_.decodedKiB.store(uint32_t(decodedBytes_ >> 10), std::memory_order_relaxed);
    return made;
}

// A bank sample's voice plays from its decoded copy, decoding further frames
// the first time it gets to them; the wrap at the end is DecodeFromMemory's.
// Returns the samples it wrote, all of them when the voice stopped (the rest
// silence); fewer when it has no copy or the sample's bytes changed by the
// loop (the rest decodes from memory).
uint32_t Mixer::PlayDecoded(Voice& voice, int16_t* out, uint32_t count) {
    uint16_t* pb = voice.pb;
    const uint32_t start = (uint32_t(pb[Pb::LoopHi]) << 16 | pb[Pb::LoopLo]) & 0x3FFFFFFFu;
    const uint32_t end = (uint32_t(pb[Pb::EndHi]) << 16 | pb[Pb::EndLo]) & 0x3FFFFFFFu;
    const uint32_t cur = (uint32_t(pb[Pb::CurHi]) << 16 | pb[Pb::CurLo]) & 0xBFFFFFFFu;
    const bool loop = pb[Pb::LoopFlag] != 0;
    PcmSample* sample = voice.pcm;

    if (!sample || sample->end != end || cur < sample->base || cur > end || (cur & 0xF) < 2) {
        sample = voice.pcm = FindDecoded(voice);

        if (!sample)
            return 0;
    }

    sample->lastUse = rendered_;
    const uint32_t last = sample->Index(end);
    uint32_t index = sample->Index(cur), i = 0;
    int32_t yn1 = int16_t(pb[Pb::AdpcmYn1]), yn2 = int16_t(pb[Pb::AdpcmYn2]);
    bool stopped = false, changed = false;

    while (i < count) {
        const uint32_t n = std::min(count - i, last + 1 - index);
        DecodeFrames(*sample, index + n);
        std::memcpy(out + i, sample->pcm.data() + index, n * sizeof(int16_t));
        i += n;
        index += n;
        yn2 = n >= 2 ? out[i - 2] : yn1;
        yn1 = out[i - 1];

        if (index <= last)
            break;

        if (!loop) {
            stopped = true;
            break;
        }

        // A stream's ring changes while it plays: from here on it decodes
        // from memory.
        if (SampleCheck(sample->data, SampleBytes(*sample), sample->coefs) != sample->check) {
            changed = true;
            break;
        }

        index = sample->Index(start);
        yn1 = int16_t(pb[Pb::LoopYn1]);
        yn2 = int16_t(pb[Pb::LoopYn2]);
    }

    // The decoder state DecodeFromMemory leaves: a stopped voice keeps the
    // last sample's history and the header after it, a wrapped one takes the
    // loop's.
    uint32_t next = start, ps;

    if (stopped) {
        ps = ByteAt(((end + 1) & ~0xFu) >> 1) & 0x7F;
    } else if (changed) {
        ps = pb[Pb::LoopPredScale] & 0x7F;
        yn1 = int16_t(pb[Pb::LoopYn1]);
        yn2 = int16_t(pb[Pb::LoopYn2]);
    } else {
        next = sample->Address(index);
        ps = ByteAt((next & ~0xFu) >> 1) & 0x7F;
    }

    pb[Pb::CurHi] = uint16_t(next >> 16);
    pb[Pb::CurLo] = uint16_t(next);
    pb[Pb::AdpcmPredScale] = uint16_t(ps);
    pb[Pb::AdpcmYn1] = uint16_t(yn1);
    pb[Pb::AdpcmYn2] = uint16_t(yn2);

    if (changed) {
        ForgetDecoded(end);
        rewritten_.insert(end);
    }

    if (stopped) {
        pb[Pb::State] = 0;
        std::fill(out + i, out + count, int16_t(0));
    }

    statistics_.decodedSamples.fetch_add(i, std::memory_order_relaxed);
    return stopped ? count : i;
}

uint32_t Mixer::Decode(Voice& voice, int16_t* out, uint32_t count) {
    uint32_t done = 0;

    if ((voice.pb[Pb::Format] & 3) == 0 && voice.pb[Pb::Type] != 1) {
        done = PlayDecoded(voice, out, count);

        if (done == count)
            return done;
    }

    statistics_.memorySamples.fetch_add(count - done, std::memory_order_relaxed);
    return done + DecodeFromMemory(voice, out + done, count - done);
}

uint32_t Mixer::DecodeFromMemory(Voice& voice, int16_t* out, uint32_t count) {
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
            const int16_t* c = table[frac >> 9];
#if defined(__ARM_NEON)
            // The rounding, saturating narrow is the scalar line's clamp.
            const int32x4_t products = vmull_s16(vld1_s16(w), vld1_s16(c));
            int32x2_t sum = vpadd_s32(vget_low_s32(products), vget_high_s32(products));
            sum = vpadd_s32(sum, sum);
            vst1_lane_s16(samples + i, vqrshrn_n_s32(vcombine_s32(sum, sum), 14), 0);
#else
            samples[i] = int16_t(Clamp16((w[0] * c[0] + w[1] * c[1] + w[2] * c[2] + w[3] * c[3] + 0x2000) >> 14));
#endif
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
            ScaleSamples(samples, ve, 0);
    } else {
        pb[Pb::VeVolume] = ScaleSamples(samples, ve, veDelta);
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

    // The voices take long enough for the reverb's lines to arrive.
    for (const auto& aux : aux_)
        if (aux)
            aux->Prefetch();

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

    // A stage reads back what it wrote a line's length earlier, and an early
    // tap `ahead` samples past the write tap reads what was written
    // length - ahead samples earlier: a block no longer than either reads
    // nothing it writes itself.
    uint32_t block = std::min(kTickSamples, config_.earlyLength);

    if (config_.preDelayLength)
        block = std::min(block, config_.preDelayLength);

    for (uint32_t k = 0; k < 3; ++k) {
        const uint32_t ahead = config_.earlyLength ? (config_.earlyTaps[k] + config_.earlyLength - config_.earlyTaps[2]) % config_.earlyLength : 0;

        if (ahead)
            block = std::min(block, config_.earlyLength - ahead);

        block = std::min({block, config_.combLength[k], config_.lastLength[k]});
    }

    for (uint32_t k = 0; k < 2; ++k)
        block = std::min(block, config_.allpassLength[k]);

    // Whole NEON steps: every block (the tick's 96 too) a multiple of 4.
    block_ = block & ~3u;
    std::fprintf(stderr, "MH3 audio: reverb blocks of %u samples (early %u taps %u/%u/%u, pre-delay %u, combs %u/%u/%u, allpasses %u/%u, last %u/%u/%u, crosstalk %.3f, level %.3f, out %.3f)\n", block_,
                 config_.earlyLength, config_.earlyTaps[0], config_.earlyTaps[1], config_.earlyTaps[2], config_.preDelayLength, config_.combLength[0], config_.combLength[1],
                 config_.combLength[2], config_.allpassLength[0], config_.allpassLength[1], config_.lastLength[0], config_.lastLength[1], config_.lastLength[2],
                 double(config_.crosstalk), double(config_.level), double(config_.outGain));
}

namespace {

uint32_t Advanced(uint32_t position, uint32_t length, uint32_t n) {
    position += n;
    return position >= length ? position - length : position;
}

// Runs `stage(line + from, at, count)` over n samples of a delay line from
// `position` on: once, or twice when the line wraps; `at` is the offset into
// the block's own arrays.
template <class Stage>
void OnLine(float* line, uint32_t length, uint32_t position, uint32_t n, Stage stage) {
    const uint32_t first = std::min(n, length - position);
    stage(line + position, 0, first);

    if (first < n)
        stage(line, first, n - first);
}

// The stages, element-wise over `count` samples (NEON, then a scalar tail).
// NEON flushes denormals, which the decaying tail would otherwise reach.

// sum (+)= tap * gain
void EarlyTap(float* sum, const float* tap, float gain, uint32_t count, bool first) {
    uint32_t i = 0;
#if defined(__ARM_NEON)
    for (; i + 4 <= count; i += 4) {
        const float32x4_t t = vld1q_f32(tap + i);
        vst1q_f32(sum + i, first ? vmulq_n_f32(t, gain) : vmlaq_n_f32(vld1q_f32(sum + i), t, gain));
    }
#endif
    for (; i < count; ++i)
        sum[i] = first ? tap[i] * gain : sum[i] + tap[i] * gain;
}

// A comb: its output adds to `through`, its input plus the fed-back output
// replaces it.
void Comb(float* line, float* through, const float* input, float gain, uint32_t count) {
    uint32_t i = 0;
#if defined(__ARM_NEON)
    for (; i + 4 <= count; i += 4) {
        const float32x4_t y = vld1q_f32(line + i);
        vst1q_f32(through + i, vaddq_f32(vld1q_f32(through + i), y));
        vst1q_f32(line + i, vmlaq_n_f32(vld1q_f32(input + i), y, gain));
    }
#endif
    for (; i < count; ++i) {
        const float y = line[i];
        through[i] += y;
        line[i] = input[i] + y * gain;
    }
}

// An allpass on `through`, in place.
void Allpass(float* line, float* through, float gain, uint32_t count) {
    uint32_t i = 0;
#if defined(__ARM_NEON)
    for (; i + 4 <= count; i += 4) {
        const float32x4_t y = vld1q_f32(line + i);
        const float32x4_t v = vmlaq_n_f32(vld1q_f32(through + i), y, gain);
        vst1q_f32(line + i, v);
        vst1q_f32(through + i, vmlsq_n_f32(y, v, gain));
    }
#endif
    for (; i < count; ++i) {
        const float y = line[i];
        const float v = through[i] + y * gain;
        line[i] = v;
        through[i] = y - v * gain;
    }
}

// The pre-delay: the delayed samples out, the new ones in.
void Exchange(float* line, float* out, const float* in, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        out[i] = line[i];
        line[i] = in[i];
    }
}

void ToFloat(float* out, const int32_t* in) {
#if defined(__ARM_NEON)
    for (uint32_t i = 0; i < kTickSamples; i += 4)
        vst1q_f32(out + i, vcvtq_f32_s32(vld1q_s32(in + i)));
#else
    for (uint32_t i = 0; i < kTickSamples; ++i)
        out[i] = float(in[i]);
#endif
}

// Truncates and saturates, as the per-sample loop's conversion.
void ToInt(int32_t* out, const float* in) {
#if defined(__ARM_NEON)
    for (uint32_t i = 0; i < kTickSamples; i += 4)
        vst1q_s32(out + i, vcvtq_s32_f32(vld1q_f32(in + i)));
#else
    for (uint32_t i = 0; i < kTickSamples; ++i)
        out[i] = in[i] >= 2147483520.0f ? INT32_MAX : in[i] <= -2147483648.0f ? INT32_MIN : int32_t(in[i]);
#endif
}

} // namespace

// The same network as ProcessSamples, a stage at a time over blocks of the
// tick, each stage in place on its delay line: no block is longer than a line
// (Reverb::Reverb), so no stage reads back what it wrote in the same block,
// and only the lowpass stays a per-sample recurrence. The stages run on NEON.
void Reverb::SetGains(const float gains[4]) {
    config_.crosstalk = gains[0];
    config_.level = gains[1];
    config_.outGain = gains[2];
    config_.sendGain = gains[3];
}

// Silent lines, as a new effect starts with.
void Reverb::Clear() {
    for (uint32_t c = 0; c < 3; ++c) {
        for (Line* line : {&early_[c], &preDelay_[c], &comb_[c][0], &comb_[c][1], &comb_[c][2], &allpass_[c][0], &allpass_[c][1], &last_[c]})
            if (line->samples)
                std::fill(line->samples.get(), line->samples.get() + line->length, 0.0f);

        lowpass_[c] = 0.0f;
        heard_[c] = false;
    }
}

void Reverb::Process(int32_t* left, int32_t* right, int32_t* surround) {
    // An output gain of 0 returns silence (the game registers the effect with
    // it and may raise it later): nothing to run. Raised again, the effect
    // starts from silent lines, its tail from before lost.
    if (config_.outGain == 0.0f) {
        for (int32_t* io : {left, right, surround})
            std::fill(io, io + kTickSamples, 0);

        idle_ = true;
        return;
    }

    if (std::exchange(idle_, false))
        Clear();

    if (block_ < 8)
        return ProcessSamples(left, right, surround);

    int32_t* io[3] = {left, right, surround};
    ReverbConfig& s = config_;
    const float coef = s.lowpassCoef, coefRest = 1.0f - coef;
    const float g = s.allpassGain, wetScale = 0.6f * s.level;
    alignas(16) float in[3][kTickSamples], wet[3][kTickSamples];

    // A channel that never had input holds silence in every line and puts
    // out exact zeros (in stereo the surround send may stay silent).
    for (uint32_t c = 0; c < 3; ++c) {
        if (!heard_[c])
            heard_[c] = std::any_of(io[c], io[c] + kTickSamples, [](int32_t v) { return v != 0; });

        if (heard_[c])
            ToFloat(in[c], io[c]);
        else
            std::fill(wet[c], wet[c] + kTickSamples, 0.0f);
    }

    for (uint32_t at = 0; at < kTickSamples; at += block_) {
        const uint32_t n = std::min(block_, kTickSamples - at);

        for (uint32_t c = 0; c < 3; ++c) {
            if (!heard_[c]) {
                s.lastPos[c] = Advanced(s.lastPos[c], s.lastLength[c], n);
                continue;
            }

            alignas(16) float delayed[kTickSamples], through[kTickSamples] = {};
            const float* x = in[c] + at;
            float* sum = wet[c] + at;
            float* e = early_[c].samples.get();

            for (uint32_t k = 0; k < 3; ++k)
                OnLine(e, s.earlyLength, s.earlyTaps[k], n, [&](float* line, uint32_t i, uint32_t count) { EarlyTap(sum + i, line, s.earlyGains[k], count, k == 0); });

            OnLine(e, s.earlyLength, s.earlyTaps[2], n, [&](float* line, uint32_t i, uint32_t count) { std::memcpy(line, x + i, count * sizeof(float)); });
            const float* input = x;

            if (s.preDelayLength) {
                OnLine(preDelay_[c].samples.get(), s.preDelayLength, s.preDelayPos, n, [&](float* line, uint32_t i, uint32_t count) { Exchange(line, delayed + i, x + i, count); });
                input = delayed;
            }

            for (uint32_t k = 0; k < 3; ++k)
                OnLine(comb_[c][k].samples.get(), s.combLength[k], s.combPos[k], n,
                       [&](float* line, uint32_t i, uint32_t count) { Comb(line, through + i, input + i, s.combGains[k], count); });

            for (uint32_t k = 0; k < 2; ++k)
                OnLine(allpass_[c][k].samples.get(), s.allpassLength[k], s.allpassPos[k], n, [&](float* line, uint32_t i, uint32_t count) { Allpass(line, through + i, g, count); });

            float lowpass = lowpass_[c];

            for (uint32_t i = 0; i < n; ++i)
                through[i] = lowpass = coefRest * through[i] + coef * lowpass;

            lowpass_[c] = lowpass;
            OnLine(last_[c].samples.get(), s.lastLength[c], s.lastPos[c], n, [&](float* line, uint32_t i, uint32_t count) { Allpass(line, through + i, g, count); });
            s.lastPos[c] = Advanced(s.lastPos[c], s.lastLength[c], n);

            for (uint32_t i = 0; i < n; ++i)
                sum[i] += through[i] * wetScale;
        }

        for (uint32_t k = 0; k < 3; ++k)
            s.earlyTaps[k] = Advanced(s.earlyTaps[k], s.earlyLength, n);

        if (s.preDelayLength)
            s.preDelayPos = Advanced(s.preDelayPos, s.preDelayLength, n);

        for (uint32_t k = 0; k < 3; ++k)
            s.combPos[k] = Advanced(s.combPos[k], s.combLength[k], n);

        for (uint32_t k = 0; k < 2; ++k)
            s.allpassPos[k] = Advanced(s.allpassPos[k], s.allpassLength[k], n);
    }

    // Crosstalk between the channels, then the output gain.
    const float cross = 0.5f * s.crosstalk;

    for (uint32_t c = 0; c < 3; ++c) {
        alignas(16) float others[kTickSamples], out[kTickSamples];
        const float* a = wet[(c + 1) % 3];
        const float* b = wet[(c + 2) % 3];

        for (uint32_t i = 0; i < kTickSamples; ++i)
            others[i] = (a[i] + b[i]) * cross;

        for (uint32_t i = 0; i < kTickSamples; ++i)
            out[i] = (wet[c][i] + others[i]) * s.outGain;

        ToInt(io[c], out);
    }
}

// Fetches the delay lines' next tick ahead: the 35 KiB of lines a channel
// cycles through every 60 ms are gone from the caches by then, and the
// stages otherwise waited on every cache line they touched.
void Reverb::Prefetch() const {
    const ReverbConfig& s = config_;

    if (s.outGain == 0.0f)
        return;

    const auto range = [](const float* line, uint32_t length, uint32_t position) {
        for (uint32_t i = 0; i < kTickSamples; i += 8) {
            const uint32_t at = position + i;
            __builtin_prefetch(line + (at < length ? at : at - length));
        }
    };

    for (uint32_t c = 0; c < 3; ++c) {
        for (uint32_t k = 0; k < 3; ++k)
            range(early_[c].samples.get(), s.earlyLength, s.earlyTaps[k]);

        if (s.preDelayLength)
            range(preDelay_[c].samples.get(), s.preDelayLength, s.preDelayPos);

        for (uint32_t k = 0; k < 3; ++k)
            range(comb_[c][k].samples.get(), s.combLength[k], s.combPos[k]);

        for (uint32_t k = 0; k < 2; ++k)
            range(allpass_[c][k].samples.get(), s.allpassLength[k], s.allpassPos[k]);

        range(last_[c].samples.get(), s.lastLength[c], s.lastPos[c]);
    }
}

void Reverb::ProcessSamples(int32_t* left, int32_t* right, int32_t* surround) {
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
