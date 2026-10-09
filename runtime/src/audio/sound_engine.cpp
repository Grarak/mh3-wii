// The game's sound engine, RMHP08 (sound_engine.h): the AX frame callback,
// the sound object list, the SE command queue, the SE manager's methods and
// the envelope, 3D, volume and pitch helpers, as native C. Addresses and
// labels are in symbols.txt; the translated originals are the port checks'
// reference (MH3_PORT_CHECK=SoundFrame,SeTick,...).
#include "sound_engine.h"

#include "g3d_native.h"
#include "guest_call.h"
#include "port_check.h"
#include "sdk_mix.h"
#include "sound_voice.h"

#include <cmath>
#include <cstdlib>

using namespace SoundEngine;

extern "C" {
void MH3_SEQRunAudioFrame(CpuContext*);
void MH3_SYNRunAudioFrame(CpuContext*);
// Translated originals (port checks).
void func_800E86FC(CpuContext*);
void func_800EDBD8(CpuContext*);
void func_800E7D98(CpuContext*);
void func_800E5FA4(CpuContext*);
void func_800E4998(CpuContext*);
void func_800EDCB4(CpuContext*);
void func_800EDE14(CpuContext*);
void func_800EDEC4(CpuContext*);
void func_800E62B4(CpuContext*);
void func_800E61CC(CpuContext*);
void func_800E5F28(CpuContext*);
void func_800E7614(CpuContext*);
void func_800E7684(CpuContext*);
void func_800E76EC(CpuContext*);
void func_800E6650(CpuContext*);
void func_800E7880(CpuContext*);
void func_800E78DC(CpuContext*);
void func_800E6378(CpuContext*);
void func_800E5DCC(CpuContext*);
void func_800E8EC4(CpuContext*);
void func_800E8FDC(CpuContext*);
void func_800E9018(CpuContext*);
void func_800E9228(CpuContext*);
void func_800E9388(CpuContext*);
void func_800E93F4(CpuContext*);
void func_800E9720(CpuContext*);
void func_800EE078(CpuContext*);
}

namespace {

// SE manager.
enum : uint32_t {
    kSeMaster = 0x08,      // s16 0..127
    kSeCategories = 0x0A,  // s16[0x31] per category
    kSeListener = 0x7C,    // MTX34* (view of the listener)
    kSeListener2 = 0x80,   // MTX34* of a second listener (split view), or 0
    kSeNear = 0x88,        // float: nearer than this, pan follows x
    kSeFront = 0x8C,       // float: the front cone's half angle (radians)
};
constexpr uint32_t kSeCategoryCount = 0x31;

// SE record (object + 0x1C); the envelope fields are sound_engine.h's.
enum : uint32_t {
    kSeLevel = 0x00,     // s16 0..127
    kSeLevel2 = 0x02,    // s16 0..127 (command 3)
    kSeVolume = 0x06,    // s16 the computed volume, 0..127
    kSeVolumeOld = 0x08, // s16 the volume MIX has
    kSeCategory = 0x0A,  // u16
    kSeRange = 0x0C,     // float: distance at which the falloff reaches 0
    kSePitch = 0x10,     // s16 cents
    kSePan = 0x12,       // s16 0..127
    kSeAuxA = 0x14,      // s16 levels
    kSeAuxB = 0x16,
    kSeAuxC = 0x18,
    kSeLowpass = 0x1A,   // u16 cutoff (Hz), 0 off
    kSeLifetime = 0x20,  // u32 ticks until release, ~0 forever
    kSeCountdown = 0x24, // u32
    kSeAge = 0x2C,       // u32 ticks
    kSeSample = 0x40,    // sample info*
    kSeVoice = 0x44,     // AXVPB*
    kSePriority = 0x48,  // s16
    kSeSPan = 0x4A,      // s16 0..127
    kSePitchUp = 0x4C,   // s16 cents
    kSePitchDown = 0x4E, // s16 cents
    kSeDistanceScale = 0x56,  // u16, x 0.001
    kSe3d = 0x58,        // u16 flags
    kSeFader = 0x5A,     // s16 level
    kSePosition = 0x5C,  // VEC3*, 0 for a 2D sound
};
enum : uint16_t { k3dVolume = 2, k3dPan = 4, k3dSPan = 8, k3dPriority = 0x10 };

// Sample info.
enum : uint32_t {
    kSampleFormat = 0x00,  // 0/1 ADPCM, 2/3 PCM16, 4/5 PCM8; odd loops
    kSampleRate = 0x04,
    kSampleLoop = 0x08,
    kSampleLoopEnd = 0x0C,
    kSampleEnd = 0x10,
    kSampleStart = 0x14,
    kSampleAdpcm = 0x18,     // AXPBADPCM, 0x28 bytes
    kSampleAdpcmLoop = 0x40, // AXPBADPCMLOOP, 3 halfwords
};

// SE command (pending queue entry): kind, sound id, s16 arguments.
enum : uint32_t { kCmdKind = 0x0C, kCmdId = 0x10, kCmdVolume = 0x18, kCmdPitch = 0x1A };
enum : uint32_t { kCmdStop = 0, kCmdPause = 1, kCmdResume = 2, kCmdVolumeSet = 3, kCmdPitchSet = 4 };

// Guest functions called by the destructors.
constexpr uint32_t kDestroyArray = 0x80457290, kDelete = 0x80040564;

int32_t Clamp127(int32_t v) {
    return v > 127 ? 127 : v < 0 ? 0 : v;
}

float Over127(int32_t v) {
    return float(v) / 127.0f;
}

// 0x800E7D38: degrees to radians (fmuls by 0.017453292f).
float Radians(float degrees) {
    return 0.017453292f * degrees;
}

// SoundListenerPolar (0x800E8F5C): the source in listener space, its angle
// atan2(x, -z) and distance. The squares and sums are single precision.
void Polar(uint32_t listener, uint32_t position, uint32_t local, float& angle, float& distance) {
    G3dNative::MtxMultVec(listener, position, local);
    const float x = FlatReadFloat32(local), y = FlatReadFloat32(local + 4), z = FlatReadFloat32(local + 8);
    angle = float(std::atan2(double(x), -double(z)));
    const float xy = x * x + y * y;
    distance = float(std::sqrt(double(z * z + xy)));
}

// 0x800E8FDC: 1 - distance / range inside the range, else 0.
float Falloff(float range, float distance) {
    if (range == 0.0f || distance > range)
        return 0.0f;
    return 1.0f - distance / range;
}

// 0x800E9388: the distance volume, max / (1 + 0.14159292 d s), in 0..max.
int32_t DistanceVolume(float distance, float scale, int16_t max) {
    const float ds = distance * scale;
    const float k = 0.14159292f * ds;
    const float d = 1.0f + k;
    const int32_t v = int32_t(float(max) / d);
    if (v > max)
        return max;
    return v < 0 ? 0 : v;
}

// 0x800E93F4: the voice priority, 31 / (1 + 0.034292035 d s), in 1..30.
int32_t DistancePriority(float distance, float scale) {
    const float ds = distance * scale;
    const float k = 0.034292035f * ds;
    const float d = 1.0f + k;
    const int32_t v = int32_t(31.0f / d);
    return v > 30 ? 30 : v < 1 ? 1 : v;
}

// 0x800E9018: the pan of a source at `angle` (0 ahead, +-pi behind): 64 in
// the middle, 32 off at the front cone's edge and at 90 degrees, back to
// the middle behind; near sources follow |x|; mixed toward the middle as
// the falloff drops.
int32_t Pan(float distance, float angle, float nearDistance, float front, float falloff, float localX) {
    const float halfPi = 1.5707964f, pi = 3.1415927f;
    const float a = std::fabs(angle);
    float offset;
    if (a >= halfPi) {
        offset = 64.0f * ((pi - a) / halfPi);
    } else if (a < front) {
        const float scale = Radians(45.0f) / front;
        offset = 64.0f * ((a / halfPi) * scale);
    } else if (0.0f == Radians(90.0f) - front) {
        offset = 64.0f;
    } else {
        const float slope = 32.0f / (Radians(90.0f) - front);
        const float t = (a - front) * slope;
        offset = 32.0f + t;
    }
    if (distance < nearDistance) {
        const float near = 0.01f * std::fabs(localX);
        offset = offset * near;
    }
    const int32_t p = angle >= 0.0f ? int32_t(64.0f + offset) : int32_t(64.0f - offset);
    const float toward = float(64 - p) * falloff;
    return Clamp127(int32_t(float(p) + toward));
}

// 0x800E9228: the surround pan from the source's z (front/back): 127 in
// mono and stereo modes; z normalized when outside -1..1.
int32_t SurroundPan(float distance, float nearDistance, float falloff, float localX, float localY, float localZ) {
    const uint32_t mode = SdkMix::SoundMode();
    if (mode == 0 || mode == 1)
        return 127;
    float z = localZ;
    if (z > 1.0f || z < -1.0f) {
        const float length = std::sqrt(localX * localX + localY * localY + localZ * localZ);
        z = localZ / length;
    }
    int32_t s = int32_t(64.0f * z);
    if (distance < nearDistance)
        s = int32_t(0.64f * localZ);
    const int32_t p = 64 - s;
    const float toward = float(64 - p) * falloff;
    return Clamp127(int32_t(float(p) + toward));
}

// 0x800E5DCC: the SE's volume: master x category x SE master x its three
// levels (level, level 2, envelope), 0..127.
int32_t SeVolume(uint32_t record) {
    const float master = Over127(S16(kMasterVolume));
    const uint32_t category = FlatRead16(record + kSeCategory);
    const float group = Over127(category < kSeCategoryCount ? S16(kSeManager + kSeCategories + category * 2) : 0);
    const float se = Over127(S16(kSeManager + kSeMaster));
    const float level = Over127(S16(record + kSeLevel)), envelope = Over127(S16(record + kEnvLevel)), level2 = Over127(S16(record + kSeLevel2));
    float v = master * se;
    v = group * v;
    v = level2 * v;
    v = envelope * v;
    v = level * v;
    v = 127.0f * v;
    const int32_t r = int32_t(v);
    if (int16_t(r) > 127)
        return 127;
    return int16_t(r) < 0 ? 0 : r;
}

// 0x800E8EC4: pitch: the sample rate / 32 kHz shifted by `cents`.
void SetPitch(uint32_t voice, uint32_t rate, int16_t cents) {
    const double shift = std::pow(2.0, double(cents) / 1200.0);
    const float base = float(rate) / 32000.0f;
    if (voice)
        SoundVoice::SetSrcRatio(voice, float(shift * double(base)));
}

// 0x800E6378: the per-tick 3D update (pan, surround pan, priority from the
// listener(s)) and the volume handed to MIX when it changed.
void Update3d(CpuContext* cpu, uint32_t manager, uint32_t record) {
    int32_t distanceVolume = 127;
    const uint32_t voice = FlatRead32(record + kSeVoice);
    if (const uint32_t position = FlatRead32(record + kSePosition)) {
        GuestFrame frame(cpu, 0x70);
        const uint32_t local = frame[44], local2 = frame[32];
        const float range = FlatReadFloat32(record + kSeRange);
        float angle, distance, angle2 = 0.0f, distance2 = 0.0f, falloff2 = 0.0f;
        Polar(FlatRead32(manager + kSeListener), position, local, angle, distance);
        const float falloff = Falloff(range, distance);
        const uint32_t listener2 = FlatRead32(manager + kSeListener2);
        if (listener2) {
            Polar(listener2, position, local2, angle2, distance2);
            falloff2 = Falloff(range, distance2);
        }
        const uint16_t flags = FlatRead16(record + kSe3d);
        const float scale = 0.001f * float(FlatRead16(record + kSeDistanceScale));
        const float nearDistance = FlatReadFloat32(manager + kSeNear), front = FlatReadFloat32(manager + kSeFront);
        if (flags & k3dVolume)
            distanceVolume = DistanceVolume(distance, scale, 127);
        if (flags & k3dPan) {
            int16_t pan = int16_t(Pan(distance, angle, nearDistance, front, falloff, FlatReadFloat32(local)));
            if (listener2) {
                const int16_t pan2 = int16_t(Pan(distance2, angle2, nearDistance, front, falloff2, FlatReadFloat32(local2)));
                pan = int16_t(pan + (pan2 - pan) / 2);
            }
            FlatWrite16(record + kSePan, uint16_t(pan));
            SdkMix::SetPan(voice, pan);
        }
        if (flags & k3dSPan) {
            int16_t span = 127;
            if (!listener2)
                span = int16_t(SurroundPan(distance, nearDistance, falloff, FlatReadFloat32(local), FlatReadFloat32(local + 4), FlatReadFloat32(local + 8)));
            FlatWrite16(record + kSeSPan, uint16_t(span));
            SdkMix::SetSPan(voice, span);
        }
        if (flags & k3dPriority) {
            const int16_t priority = int16_t(DistancePriority(distance, scale));
            FlatWrite16(record + kSePriority, uint16_t(priority));
            SoundVoice::SetPriority(voice, uint32_t(int32_t(priority)));
        }
    }
    const int16_t volume = int16_t(SeVolume(record));
    const int32_t v = int32_t((float(distanceVolume) / 127.0f) * float(volume));
    FlatWrite16(record + kSeVolume, uint16_t(v));
    if (int16_t(v) != S16(record + kSeVolumeOld)) {
        FlatWrite16(record + kSeVolume, uint16_t(Clamp127(int16_t(v))));
        SdkMix::SetInput(voice, Level(S16(record + kSeVolume)));
    }
    FlatWrite16(record + kSeVolumeOld, FlatRead16(record + kSeVolume));
}

// 0x800EE078: points the voice at the sample (format, addresses, ADPCM
// state, rate shifted by `cents`) and sets its lowpass.
void SetupVoice(uint32_t sample, uint32_t voice, int16_t cents, uint16_t lowpass) {
    const double shift = std::pow(2.0, double(cents) / 1200.0);
    const float rate = float(FlatRead32(sample + kSampleRate)) * float(shift);
    const double fixed = 65536.0 * double(rate / 32000.0f);
    const uint32_t ratio = fixed > 0.0 ? (fixed >= 4294967296.0 ? 0xFFFFFFFFu : uint32_t(fixed)) : 0;
    SoundVoice::SetSrcType(voice, 2);
    const uint32_t format = FlatRead32(sample + kSampleFormat);
    if (format <= 5) {
        static constexpr uint16_t kFormats[3] = {0, 10, 0x19};
        static constexpr uint16_t kGains[3] = {0, 0x800, 0x100};
        const bool loops = format & 1;
        SoundVoice::Addr addr;
        addr.loopFlag = loops;
        addr.format = kFormats[format / 2];
        addr.loop = FlatRead32(sample + kSampleLoop);
        addr.end = FlatRead32(sample + (loops ? kSampleLoopEnd : kSampleEnd));
        addr.current = FlatRead32(sample + kSampleStart);
        SoundVoice::SetAddr(voice, addr);
        SoundVoice::Adpcm adpcm{};
        if (format < 2) {
            for (uint32_t i = 0; i < 16; ++i)
                adpcm.coefs[i] = FlatRead16(sample + kSampleAdpcm + i * 2);
            adpcm.gain = FlatRead16(sample + kSampleAdpcm + 0x20);
            adpcm.predScale = FlatRead16(sample + kSampleAdpcm + 0x22);
            adpcm.yn1 = FlatRead16(sample + kSampleAdpcm + 0x24);
            adpcm.yn2 = FlatRead16(sample + kSampleAdpcm + 0x26);
        } else {
            adpcm.gain = kGains[format / 2];
        }
        SoundVoice::SetAdpcm(voice, adpcm);
        SoundVoice::SetSrc(voice, ratio);
        if (format == 1)
            SoundVoice::SetAdpcmLoop(voice, FlatRead16(sample + kSampleAdpcmLoop), FlatRead16(sample + kSampleAdpcmLoop + 2), FlatRead16(sample + kSampleAdpcmLoop + 4));
    }
    uint16_t a0 = 0, b0 = 0;
    if (lowpass)
        SoundVoice::LpfCoefs(lowpass, a0, b0);
    SoundVoice::SetLpf(voice, lowpass != 0, 0, a0, b0);
}

// ---------------------------------------------------------------------------
// SE manager methods (vtable 0x80598568).
// ---------------------------------------------------------------------------

// +0x24, 0x800E6650: starts the voice the envelope's first step asked for.
void SeStart(uint32_t object) {
    const uint32_t record = Record(object), voice = FlatRead32(record + kSeVoice);
    SetupVoice(FlatRead32(record + kSeSample), voice, S16(record + kSePitch), FlatRead16(record + kSeLowpass));
    const int32_t fader = Level(S16(record + kSeFader)), auxC = Level(S16(record + kSeAuxC)), auxB = Level(S16(record + kSeAuxB));
    const int32_t auxA = Level(S16(record + kSeAuxA)), input = Level(S16(record + kSeVolume));
    SdkMix::InitChannel(voice, MixMode(), input, uint32_t(auxA), uint32_t(auxB), uint32_t(auxC), uint32_t(int32_t(S16(record + kSePan))),
                        uint32_t(int32_t(S16(record + kSeSPan))), uint32_t(fader));
    SoundVoice::SetState(voice, 1);
    Set(object, kStarted);
    FlatWrite8(record + kEnvState, kEnvAttacking);
}

// +0x18, 0x800E7614.
void SeStop(uint32_t object) {
    if (!object)
        return;
    Clear(object, kPaused);
    Clear(object, kTicking);
    Set(object, kReleased);
    if (const uint32_t voice = FlatRead32(Record(object) + kSeVoice))
        SoundVoice::SetState(voice, 0);
}

// +0x1C, 0x800E7684.
void SePause(uint32_t object) {
    if (!object)
        return;
    if (const uint32_t voice = FlatRead32(Record(object) + kSeVoice)) {
        Set(object, kPaused);
        SoundVoice::SetState(voice, 0);
    }
}

// +0x20, 0x800E76EC.
void SeResume(uint32_t object) {
    if (!object || !Has(object, kPaused))
        return;
    Clear(object, kPaused);
    const uint32_t voice = FlatRead32(Record(object) + kSeVoice);
    if (voice && SoundVoice::State(voice) == 0)
        SoundVoice::SetState(voice, 1);
}

// +0x0C, 0x800E62B4: countdown, lifetime, envelope and 3D/volume.
void SeTick(CpuContext* cpu, uint32_t manager, uint32_t object) {
    const uint32_t record = Record(object);
    const uint32_t countdown = FlatRead32(record + kSeCountdown);
    if (countdown != 0xFFFFFFFFu && countdown != 0)
        FlatWrite32(record + kSeCountdown, countdown - 1);
    const uint32_t lifetime = FlatRead32(record + kSeLifetime);
    if (lifetime != 0xFFFFFFFFu) {
        const uint32_t age = FlatRead32(record + kSeAge) + 1;
        FlatWrite32(record + kSeAge, age);
        if (lifetime <= age && FlatRead8(record + kEnvState) != kEnvStart) {
            FlatWrite8(record + kEnvState, kEnvReleasing);
            FlatWrite32(record + kSeLifetime, 0xFFFFFFFFu);
        }
    }
    Envelope(cpu, manager, object, record);
    Update3d(cpu, manager, record);
}

// +0x10, 0x800E61CC: frees a released object, or one whose voice finished;
// true (and `object` the next active one) when it did.
bool SeUpdate(uint32_t& object) {
    const uint32_t record = Record(object), voice = FlatRead32(record + kSeVoice);
    if (Has(object, kReleased)) {
        if (voice)
            SoundVoice::Free(voice);
        object = FreeObject(object);
        return true;
    }
    if (Has(object, kPaused) || !Has(object, kStarted) || SoundVoice::State(voice) != 0)
        return false;
    SoundVoice::Free(voice);
    object = FreeObject(object);
    return true;
}

} // namespace

// +0x14, 0x800E5F28: AX stole the voice.
void SoundEngine::SeVoiceDropped(uint32_t, uint32_t object) {
    const uint32_t record = Record(object);
    FlatWrite32(record + kSeVoice, 0);
    FlatWrite32(record + kSeSample, 0);
    Clear(object, kTicking);
    Set(object, kReleased);
}

namespace {

// 0x800E7880: command 3, the second level.
void SeSetLevel2(uint32_t object, int16_t level) {
    if (object)
        FlatWrite16(Record(object) + kSeLevel2, uint16_t(Clamp127(level)));
}

// 0x800E78DC: command 4, the pitch within the record's range.
void SeSetPitch(uint32_t object, int16_t cents) {
    if (!object)
        return;
    const uint32_t record = Record(object);
    int32_t v = cents;
    if (v > S16(record + kSePitchUp))
        v = S16(record + kSePitchUp);
    else if (v < -S16(record + kSePitchDown))
        v = -S16(record + kSePitchDown);
    FlatWrite16(record + kSePitch, uint16_t(v));
    SetPitch(FlatRead32(record + kSeVoice), FlatRead32(FlatRead32(record + kSeSample) + kSampleRate), S16(record + kSePitch));
}

// ---------------------------------------------------------------------------
// The AX frame.
// ---------------------------------------------------------------------------

bool IsStream(uint32_t manager) {
    return manager == kStreamManager;
}

// 0x800EDBD8: every active object: its manager frees it, or ticks it.
void UpdateObjects(CpuContext* cpu) {
    uint32_t object = FlatRead32(kActive + kNext);
    while (object != kActive) {
        if (FlatRead8(object + kObjType) == kTypeStream) {
            if (Stream::Update(cpu, kStreamManager, object))
                continue;
            if (Has(object, kTicking))
                Stream::Tick(cpu, kStreamManager, object);
        } else {
            if (SeUpdate(object))
                continue;
            if (Has(object, kTicking))
                SeTick(cpu, kSeManager, object);
        }
        object = FlatRead32(object + kNext);
    }
}

// 0x800EDCB4 / 0x800EDE14 / 0x800EDEC4: stop, pause or resume every object
// of a type (1 SE, 2 stream).
template <void (*Method)(CpuContext*, uint32_t, uint32_t)>
void ForEachOfType(CpuContext* cpu, uint32_t type) {
    const uint32_t manager = type == kTypeStream ? kStreamManager : kSeManager;
    for (uint32_t object = FlatRead32(kActive + kNext); object != kActive; object = FlatRead32(object + kNext))
        if (FlatRead8(object + kObjType) == type)
            Method(cpu, manager, object);
}

// 0x800E5FA4: the SE commands queued since the last frame, by sound id.
void RunCommands(CpuContext* cpu) {
    uint32_t command = FlatRead32(kPending + kNext);
    while (command != kPending) {
        const uint32_t kind = FlatRead32(command + kCmdKind), id = FlatRead32(command + kCmdId);
        for (uint32_t object = FlatRead32(kActive + kNext); object != kActive; object = FlatRead32(object + kNext)) {
            if (FlatRead8(object + kObjType) != kTypeSe || FlatRead32(object + kObjId) != id)
                continue;
            switch (kind) {
            case kCmdStop:
                ManagerStop(cpu, kSeManager, object);
                break;
            case kCmdPause:
                ManagerPause(cpu, kSeManager, object);
                break;
            case kCmdResume:
                ManagerResume(cpu, kSeManager, object);
                break;
            case kCmdVolumeSet:
                SeSetLevel2(object, S16(command + kCmdVolume));
                break;
            case kCmdPitchSet:
                SeSetPitch(object, S16(command + kCmdPitch));
                break;
            }
        }
        command = MoveTo(kCommands, command);
    }
}

// A manager's global stop/pause/resume request (one per frame, stop first).
void ServeRequests(CpuContext* cpu, uint32_t manager, uint32_t type) {
    const uint32_t requests = FlatRead32(manager + kMgrRequests);
    uint32_t served;
    if (requests & kStopAll) {
        ForEachOfType<ManagerStop>(cpu, type);
        served = kStopAll;
    } else if (requests & kPauseAll) {
        ForEachOfType<ManagerPause>(cpu, type);
        served = kPauseAll;
    } else if (requests & kResumeAll) {
        ForEachOfType<ManagerResume>(cpu, type);
        served = kResumeAll;
    } else {
        return;
    }
    FlatWrite32(manager + kMgrRequests, FlatRead32(manager + kMgrRequests) & ~served);
}

// 0x800E7D98: the SE side of the frame.
void SeFrame(CpuContext* cpu) {
    RunCommands(cpu);
    ServeRequests(cpu, kSeManager, kTypeSe);
}

// 0x800E4914: the stream gains move one step (the value becomes the
// previous one, the new one the value).
void StepStreamGains() {
    FlatWrite32(kStreamGainAOld, FlatRead32(kStreamGainA));
    FlatWrite32(kStreamGainA, FlatRead32(kStreamGainANew));
    FlatWrite32(kStreamGainBOld, FlatRead32(kStreamGainB));
    FlatWrite32(kStreamGainB, FlatRead32(kStreamGainBNew));
}

// 0x800E4998: the stream side of the frame.
void StreamFrame(CpuContext* cpu) {
    StepStreamGains();
    ServeRequests(cpu, kStreamManager, kTypeStream);
}

// 0x800E86FC: the game's AX frame callback.
void Frame(CpuContext* cpu) {
    MH3_SEQRunAudioFrame(cpu);
    MH3_SYNRunAudioFrame(cpu);
    SdkMix::UpdateSettings();
    UpdateObjects(cpu);
    SeFrame(cpu);
    StreamFrame(cpu);
}

// ---------------------------------------------------------------------------
// Entry points.
// ---------------------------------------------------------------------------

float F(CpuContext* cpu, int i) {
    return float(cpu->fpr[i].d);
}

void FramePort(CpuContext* cpu) {
    Frame(cpu);
}
void UpdateObjectsPort(CpuContext* cpu) {
    UpdateObjects(cpu);
}
void SeFramePort(CpuContext* cpu) {
    SeFrame(cpu);
}
void RunCommandsPort(CpuContext* cpu) {
    RunCommands(cpu);
}
void StreamFramePort(CpuContext* cpu) {
    StreamFrame(cpu);
}
void StopAllPort(CpuContext* cpu) {
    ForEachOfType<ManagerStop>(cpu, cpu->gpr[4] & 0xFF);
}
void PauseAllPort(CpuContext* cpu) {
    ForEachOfType<ManagerPause>(cpu, cpu->gpr[4] & 0xFF);
}
void ResumeAllPort(CpuContext* cpu) {
    ForEachOfType<ManagerResume>(cpu, cpu->gpr[4] & 0xFF);
}
void SeTickPort(CpuContext* cpu) {
    SeTick(cpu, cpu->gpr[3], cpu->gpr[4]);
}
void SeUpdatePort(CpuContext* cpu) {
    uint32_t object = FlatRead32(cpu->gpr[4]);
    const bool freed = SeUpdate(object);
    if (freed)
        FlatWrite32(cpu->gpr[4], object);
    cpu->gpr[3] = freed;
}
void SeVoiceDroppedPort(CpuContext* cpu) {
    SeVoiceDropped(cpu->gpr[4], SoundVoice::Context(cpu->gpr[4]));
}
// 0x800E5738: the SE voices' AX drop callback (r3 the voice).
void SeVoiceCallbackPort(CpuContext* cpu) {
    SeVoiceDropped(cpu->gpr[3], SoundVoice::Context(cpu->gpr[3]));
}
void SeStopPort(CpuContext* cpu) {
    SeStop(cpu->gpr[4]);
}
void SePausePort(CpuContext* cpu) {
    SePause(cpu->gpr[4]);
}
void SeResumePort(CpuContext* cpu) {
    SeResume(cpu->gpr[4]);
}
void SeStartPort(CpuContext* cpu) {
    SeStart(cpu->gpr[4]);
}
void SeSetLevel2Port(CpuContext* cpu) {
    SeSetLevel2(cpu->gpr[4], int16_t(cpu->gpr[5]));
}
void SeSetPitchPort(CpuContext* cpu) {
    SeSetPitch(cpu->gpr[4], int16_t(cpu->gpr[5]));
}
void Update3dPort(CpuContext* cpu) {
    Update3d(cpu, cpu->gpr[3], cpu->gpr[4]);
}
void SeVolumePort(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(SeVolume(cpu->gpr[3]));
}
void SetPitchPort(CpuContext* cpu) {
    SetPitch(cpu->gpr[3], cpu->gpr[4], int16_t(cpu->gpr[5]));
}
void FalloffPort(CpuContext* cpu) {
    FlatWriteFloat32(cpu->gpr[5], Falloff(FlatReadFloat32(cpu->gpr[3]), FlatReadFloat32(cpu->gpr[4])));
}
void PanPort(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Pan(F(cpu, 1), F(cpu, 2), F(cpu, 3), F(cpu, 4), F(cpu, 5), FlatReadFloat32(cpu->gpr[3])));
}
void SurroundPanPort(CpuContext* cpu) {
    const uint32_t v = cpu->gpr[3];
    cpu->gpr[3] = uint32_t(SurroundPan(F(cpu, 1), F(cpu, 2), F(cpu, 3), FlatReadFloat32(v), FlatReadFloat32(v + 4), FlatReadFloat32(v + 8)));
}
void DistanceVolumePort(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(DistanceVolume(F(cpu, 1), F(cpu, 2), int16_t(cpu->gpr[3])));
}
void DistancePriorityPort(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(DistancePriority(F(cpu, 1), F(cpu, 2)));
}
void EnvelopePort(CpuContext* cpu) {
    Envelope(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
}
void SetupVoicePort(CpuContext* cpu) {
    SetupVoice(cpu->gpr[3], cpu->gpr[4], int16_t(cpu->gpr[5]), uint16_t(cpu->gpr[6]));
}

// Destructors (vtable +0x08 and the base classes'): r3 this, r4 > 0 frees it.
void DeleteIf(CpuContext* cpu, uint32_t self, int16_t flags) {
    if (flags > 0)
        GuestCall{cpu}(kDelete, {self});
}

} // namespace

namespace SoundEngine {

#ifdef MH3_DEVELOPMENT
static bool RunOriginal(const char* name) {
    static const bool originals = std::getenv("MH3_SOUND_ORIGINALS") != nullptr;
    return originals && !PortCheck::Enabled(name);
}
#endif

void Entry(CpuContext* cpu, const char* name, void (*original)(CpuContext*), void (*port)(CpuContext*)) {
#ifdef MH3_DEVELOPMENT
    if (RunOriginal(name)) {
        original(cpu);
        return;
    }
#endif
    PortCheck::Call(cpu, name, original, port);
}

void EntryPure(CpuContext* cpu, const char* name, void (*original)(CpuContext*), void (*port)(CpuContext*)) {
#ifdef MH3_DEVELOPMENT
    if (RunOriginal(name)) {
        original(cpu);
        return;
    }
    if (PortCheck::Enabled(name)) {
        PortCheck::RunPure(cpu, name, PortCheck::Returns::Gpr3, original, port);
        return;
    }
#endif
    port(cpu);
}

uint32_t FreeObject(uint32_t object) {
    FlatWrite32(object + kObjFlags, 0);
    FlatWrite32(object + kObjId, 0);
    FlatWrite8(object + kObjSlot, 0xFF);
    FlatWrite32(object + kObjOwner, 0);
    FlatWrite8(object + kObjType, 0);
    return MoveTo(kObjects, object);
}

void Envelope(CpuContext* cpu, uint32_t manager, uint32_t object, uint32_t envelope) {
    const auto level = [&] { return S16(envelope + kEnvLevel); };
    const auto setLevel = [&](int32_t v) { FlatWrite16(envelope + kEnvLevel, uint16_t(v)); };
    const auto setState = [&](uint8_t s) { FlatWrite8(envelope + kEnvState, s); };
    switch (FlatRead8(envelope + kEnvState)) {
    case kEnvStart:
        if (IsStream(manager))
            Stream::Start(manager, object);
        else
            SeStart(object);
        return;
    case kEnvAttacking:
        setLevel(level() + S16(envelope + kEnvAttack));
        if (level() < 127)
            return;
        setLevel(127);
        setState(S16(envelope + kEnvDecay) ? kEnvDecaying : S16(envelope + kEnvSlide) ? kEnvSliding : kEnvHolding);
        return;
    case kEnvDecaying:
        setLevel(level() - S16(envelope + kEnvDecay));
        if (level() > S16(envelope + kEnvSustain))
            return;
        setLevel(S16(envelope + kEnvSustain));
        setState(S16(envelope + kEnvSlide) ? kEnvSliding : kEnvHolding);
        return;
    case kEnvSliding:
        setLevel(level() + S16(envelope + kEnvSlide));
        if (level() >= 127) {
            setLevel(127);
            setState(kEnvHolding);
        } else if (level() <= 0) {
            setLevel(0);
            setState(kEnvReleasing);
        }
        return;
    case kEnvReleasing:
        setLevel(level() - S16(envelope + kEnvRelease));
        if (level() > 0)
            return;
        setLevel(0);
        ManagerStop(cpu, manager, object);
        return;
    default:
        return;
    }
}

void ManagerStop(CpuContext* cpu, uint32_t manager, uint32_t object) {
    if (IsStream(manager))
        Stream::Stop(cpu, manager, object);
    else
        SeStop(object);
}

void ManagerPause(CpuContext*, uint32_t manager, uint32_t object) {
    if (IsStream(manager))
        Stream::Pause(object);
    else
        SePause(object);
}

void ManagerResume(CpuContext*, uint32_t manager, uint32_t object) {
    if (IsStream(manager))
        Stream::Resume(object);
    else
        SeResume(object);
}

} // namespace SoundEngine

extern "C" {

void MH3_SoundFrame(CpuContext* cpu) {
    Entry(cpu, "SoundFrame", func_800E86FC, FramePort);
}
void MH3_SoundUpdateObjects(CpuContext* cpu) {
    Entry(cpu, "SoundUpdateObjects", func_800EDBD8, UpdateObjectsPort);
}
void MH3_SeFrame(CpuContext* cpu) {
    Entry(cpu, "SeFrame", func_800E7D98, SeFramePort);
}
void MH3_SeRunCommands(CpuContext* cpu) {
    Entry(cpu, "SeRunCommands", func_800E5FA4, RunCommandsPort);
}
void MH3_StreamFrame(CpuContext* cpu) {
    Entry(cpu, "StreamFrame", func_800E4998, StreamFramePort);
}
void MH3_SoundStopAll(CpuContext* cpu) {
    Entry(cpu, "SoundStopAll", func_800EDCB4, StopAllPort);
}
void MH3_SoundPauseAll(CpuContext* cpu) {
    Entry(cpu, "SoundPauseAll", func_800EDE14, PauseAllPort);
}
void MH3_SoundResumeAll(CpuContext* cpu) {
    Entry(cpu, "SoundResumeAll", func_800EDEC4, ResumeAllPort);
}
void MH3_SeTick(CpuContext* cpu) {
    Entry(cpu, "SeTick", func_800E62B4, SeTickPort);
}
void MH3_SeUpdate(CpuContext* cpu) {
    Entry(cpu, "SeUpdate", func_800E61CC, SeUpdatePort);
}
void MH3_SeVoiceDropped(CpuContext* cpu) {
    Entry(cpu, "SeVoiceDropped", func_800E5F28, SeVoiceDroppedPort);
}
void MH3_SeVoiceCallback(CpuContext* cpu) {
    SeVoiceCallbackPort(cpu);
}
void MH3_SeStop(CpuContext* cpu) {
    Entry(cpu, "SeStop", func_800E7614, SeStopPort);
}
void MH3_SePause(CpuContext* cpu) {
    Entry(cpu, "SePause", func_800E7684, SePausePort);
}
void MH3_SeResume(CpuContext* cpu) {
    Entry(cpu, "SeResume", func_800E76EC, SeResumePort);
}
void MH3_SeStart(CpuContext* cpu) {
    Entry(cpu, "SeStart", func_800E6650, SeStartPort);
}
void MH3_SeSetLevel2(CpuContext* cpu) {
    Entry(cpu, "SeSetLevel2", func_800E7880, SeSetLevel2Port);
}
void MH3_SeSetPitch(CpuContext* cpu) {
    Entry(cpu, "SeSetPitch", func_800E78DC, SeSetPitchPort);
}
void MH3_SoundVoiceUpdate3D(CpuContext* cpu) {
    Entry(cpu, "SoundVoiceUpdate3D", func_800E6378, Update3dPort);
}
void MH3_SeVolume(CpuContext* cpu) {
    EntryPure(cpu, "SeVolume", func_800E5DCC, SeVolumePort);
}
void MH3_SoundSetPitch(CpuContext* cpu) {
    Entry(cpu, "SoundSetPitch", func_800E8EC4, SetPitchPort);
}
void MH3_SoundFalloff(CpuContext* cpu) {
    Entry(cpu, "SoundFalloff", func_800E8FDC, FalloffPort);
}
void MH3_SoundPan(CpuContext* cpu) {
    EntryPure(cpu, "SoundPan", func_800E9018, PanPort);
}
void MH3_SoundSurroundPan(CpuContext* cpu) {
    EntryPure(cpu, "SoundSurroundPan", func_800E9228, SurroundPanPort);
}
void MH3_SoundDistanceVolume(CpuContext* cpu) {
    EntryPure(cpu, "SoundDistanceVolume", func_800E9388, DistanceVolumePort);
}
void MH3_SoundDistancePriority(CpuContext* cpu) {
    EntryPure(cpu, "SoundDistancePriority", func_800E93F4, DistancePriorityPort);
}
void MH3_SoundEnvelope(CpuContext* cpu) {
    Entry(cpu, "SoundEnvelope", func_800E9720, EnvelopePort);
}
void MH3_SeSetupVoice(CpuContext* cpu) {
    Entry(cpu, "SeSetupVoice", func_800EE078, SetupVoicePort);
}

// SoundListenerPolar, 0x800E8F5C (r3 listener, r4 position, r5 local out, r6
// angle out, r7 distance out).
void MH3_SoundListenerPolar(CpuContext* cpu) {
    float angle, distance;
    Polar(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], angle, distance);
    FlatWriteFloat32(cpu->gpr[6], angle);
    FlatWriteFloat32(cpu->gpr[7], distance);
}

// Destructors.
void MH3_SeManagerDestroy(CpuContext* cpu) {  // 0x800E5494
    const uint32_t self = cpu->gpr[3];
    const int16_t flags = int16_t(cpu->gpr[4]);
    if (self) {
        GuestFrame frame(cpu, 0x10);
        GuestCall call{cpu};
        call(kDestroyArray, {self + 0x90, 0x800E55AC, 0x48, 0x31});
        call(0x800E550C, {self, 0});
        DeleteIf(cpu, self, flags);
    }
    cpu->gpr[3] = self;
}
void MH3_SoundManagerDestroy(CpuContext* cpu) {  // 0x800E550C
    const uint32_t self = cpu->gpr[3];
    const int16_t flags = int16_t(cpu->gpr[4]);
    if (self) {
        GuestFrame frame(cpu, 0x10);
        GuestCall{cpu}(0x800E5568, {self, 0});
        DeleteIf(cpu, self, flags);
    }
    cpu->gpr[3] = self;
}
void MH3_SoundManagerBaseDestroy(CpuContext* cpu) {  // 0x800E5568, 0x800ED8AC
    const uint32_t self = cpu->gpr[3];
    if (self && int16_t(cpu->gpr[4]) > 0) {
        GuestFrame frame(cpu, 0x10);
        DeleteIf(cpu, self, int16_t(cpu->gpr[4]));
    }
    cpu->gpr[3] = self;
}
void MH3_StreamManagerDestroy(CpuContext* cpu) {  // 0x800E9DBC
    const uint32_t self = cpu->gpr[3];
    const int16_t flags = int16_t(cpu->gpr[4]);
    if (self) {
        GuestFrame frame(cpu, 0x10);
        GuestCall call{cpu};
        call(kDestroyArray, {self + 0x5C, 0x800E9E34, 0x28, 3});
        call(0x800E550C, {self, 0});
        DeleteIf(cpu, self, flags);
    }
    cpu->gpr[3] = self;
}
void MH3_StreamManagerBaseDestroy(CpuContext* cpu) {  // 0x800ED850
    const uint32_t self = cpu->gpr[3];
    const int16_t flags = int16_t(cpu->gpr[4]);
    if (self) {
        GuestFrame frame(cpu, 0x10);
        GuestCall{cpu}(0x800ED8AC, {self, 0});
        DeleteIf(cpu, self, flags);
    }
    cpu->gpr[3] = self;
}

// Accessors and list helpers (r3 result).
void MH3_SoundClearRequests(CpuContext* cpu) {  // 0x800E4A44 (manager, bits)
    FlatWrite32(cpu->gpr[3] + kMgrRequests, FlatRead32(cpu->gpr[3] + kMgrRequests) & ~cpu->gpr[4]);
}
void MH3_SoundObjects(CpuContext* cpu) {  // 0x800E4A58
    cpu->gpr[3] = kObjects;
}
void MH3_SoundHasRequest(CpuContext* cpu) {  // 0x800E4A64 (manager, bits)
    cpu->gpr[3] = (FlatRead32(cpu->gpr[3] + kMgrRequests) & cpu->gpr[4]) != 0;
}
void MH3_StreamManager(CpuContext* cpu) {  // 0x800E4A7C
    cpu->gpr[3] = kStreamManager;
}
void MH3_StepStreamGains(CpuContext*) {  // 0x800E4914
    StepStreamGains();
}
void MH3_StreamGainChanged(CpuContext* cpu) {  // 0x800E4940
    cpu->gpr[3] = 1;
}
void MH3_StreamGain(CpuContext* cpu) {  // 0x800E4948 (category byte) -> f1
    cpu->fpr[1].d = FlatReadFloat32(FlatRead8(kStreamGainSelect + (cpu->gpr[3] & 0xFF)) ? kStreamGainB : kStreamGainA);
}
void MH3_SeManager(CpuContext* cpu) {  // 0x800E5778
    cpu->gpr[3] = kSeManager;
}
void MH3_SoundSetFlags(CpuContext* cpu) {  // 0x800E5F7C (object, bits)
    Set(cpu->gpr[3], cpu->gpr[4]);
}
void MH3_SoundClearFlags(CpuContext* cpu) {  // 0x800E5F8C (object, bits)
    Clear(cpu->gpr[3], cpu->gpr[4]);
}
void MH3_SoundRecord(CpuContext* cpu) {  // 0x800E5F9C, 0x800EA840
    cpu->gpr[3] = Record(cpu->gpr[3]);
}
void MH3_SeCommandsEnd(CpuContext* cpu) {  // 0x800E6118 (queue, entry): entry is the pending list's head node
    cpu->gpr[3] = cpu->gpr[3] + 0x4C == cpu->gpr[4];
}
void MH3_SoundSame(CpuContext* cpu) {  // 0x800E614C, 0x800E6198
    cpu->gpr[3] = cpu->gpr[3] == cpu->gpr[4];
}
void MH3_SeCommandsPending(CpuContext* cpu) {  // 0x800E615C, 0x800E9C6C
    cpu->gpr[3] += 0x4C;
}
void MH3_SoundObjectsEnd(CpuContext* cpu) {  // 0x800E6164 (list, object)
    cpu->gpr[3] = cpu->gpr[3] + 0x110 == cpu->gpr[4];
}
void MH3_SoundObjectsActive(CpuContext* cpu) {  // 0x800E61A8, 0x800EDBAC
    cpu->gpr[3] += 0x110;
}
void MH3_SoundListNext(CpuContext* cpu) {  // 0x800E61B0, 0x800E9C40
    cpu->gpr[3] = FlatRead32(cpu->gpr[3] + kNext);
}
void MH3_SeCommandArgs(CpuContext* cpu) {  // 0x800E61B8
    cpu->gpr[3] += 0x0C;
}
void MH3_SeCommands(CpuContext* cpu) {  // 0x800E61C0
    cpu->gpr[3] = kCommands;
}
void MH3_SoundHasFlags(CpuContext* cpu) {  // 0x800E629C (object, bits)
    cpu->gpr[3] = Has(cpu->gpr[3], cpu->gpr[4]);
}
void MH3_SoundMixMode(CpuContext* cpu) {  // 0x800E672C (config)
    cpu->gpr[3] = FlatRead32(cpu->gpr[3] + 0xBEC);
}
void MH3_SoundMixConfig(CpuContext* cpu) {  // 0x800E6734
    cpu->gpr[3] = kMixConfig;
}
void MH3_SeMasterVolume(CpuContext* cpu) {  // 0x800E8590
    cpu->gpr[3] = uint32_t(int32_t(S16(kSeManager + kSeMaster)));
}
void MH3_SeManagerMasterVolume(CpuContext* cpu) {  // 0x800E85B4 (manager)
    cpu->gpr[3] = uint32_t(int32_t(S16(cpu->gpr[3] + kSeMaster)));
}
void MH3_SeCategoryVolume(CpuContext* cpu) {  // 0x800E85F8 (category)
    const uint32_t category = cpu->gpr[3];
    cpu->gpr[3] = category < kSeCategoryCount ? uint32_t(int32_t(S16(kSeManager + kSeCategories + category * 2))) : 0;
}
void MH3_SeManagerCategoryVolume(CpuContext* cpu) {  // 0x800E863C (manager, category)
    cpu->gpr[3] = uint32_t(int32_t(S16(cpu->gpr[3] + kSeCategories + cpu->gpr[4] * 2)));
}
void MH3_SoundMasterVolumeAddress(CpuContext* cpu) {  // 0x800E86F4
    cpu->gpr[3] = kMasterVolume;
}
void MH3_SoundMixSoundMode(CpuContext* cpu) {  // 0x800E88BC
    cpu->gpr[3] = SdkMix::SoundMode();
}
void MH3_SoundMasterVolume(CpuContext* cpu) {  // 0x800E88C0
    cpu->gpr[3] = uint32_t(int32_t(S16(kMasterVolume)));
}
void MH3_SoundReadS16(CpuContext* cpu) {  // 0x800E88E4
    cpu->gpr[3] = uint32_t(int32_t(S16(cpu->gpr[3])));
}
void MH3_SoundLevel(CpuContext* cpu) {  // 0x800E8EAC (level)
    cpu->gpr[3] = uint32_t(Level(int16_t(cpu->gpr[3])));
}
void MH3_SoundRadians(CpuContext* cpu) {  // 0x800E7D38
    cpu->fpr[1].d = Radians(F(cpu, 1));
}
void MH3_SoundListInsert(CpuContext* cpu) {  // 0x800E9A74, 0x800ED9A0 (at, node)
    InsertAfter(cpu->gpr[3], cpu->gpr[4]);
}
void MH3_SoundListMoveTo(CpuContext* cpu) {  // 0x800E9BC0, 0x800E9BC4, 0x800EDB0C (list, node) -> next
    cpu->gpr[3] = MoveTo(cpu->gpr[3], cpu->gpr[4]);
}
void MH3_SoundListUnlink(CpuContext* cpu) {  // 0x800E9C24, 0x800EDB6C
    Unlink(cpu->gpr[3]);
}
void MH3_SeCommandsFirst(CpuContext* cpu) {  // 0x800E9C48 (queue)
    cpu->gpr[3] = FlatRead32(cpu->gpr[3] + 0x4C + kNext);
}
void MH3_SoundObjectsFirst(CpuContext* cpu) {  // 0x800EDB88 (list)
    cpu->gpr[3] = FlatRead32(cpu->gpr[3] + 0x110 + kNext);
}
void MH3_SoundFreeObject(CpuContext* cpu) {  // 0x800EDAEC (list, object) -> next
    cpu->gpr[3] = FreeObject(cpu->gpr[4]);
}
void MH3_SoundCopyAdpcmLoop(CpuContext* cpu) {  // 0x800EE56C (dst, src): 3 halfwords
    for (uint32_t i = 0; i < 6; i += 2)
        FlatWrite16(cpu->gpr[3] + i, FlatRead16(cpu->gpr[4] + i));
}
void MH3_SoundCopyAdpcm(CpuContext* cpu) {  // 0x800EE588 (dst, src): 0x28 bytes
    for (uint32_t i = 0; i < 0x28; i += 2)
        FlatWrite16(cpu->gpr[3] + i, FlatRead16(cpu->gpr[4] + i));
}

} // extern "C"

REGISTER_NATIVE_FUNCTION_AS(0x800E86FC, MH3_SoundFrame, "SoundAxFrameCallback");
REGISTER_NATIVE_FUNCTION_AS(0x800EDBD8, MH3_SoundUpdateObjects, "SoundUpdateObjects");
REGISTER_NATIVE_FUNCTION_AS(0x800E7D98, MH3_SeFrame, "SeFrame");
REGISTER_NATIVE_FUNCTION_AS(0x800E5FA4, MH3_SeRunCommands, "SeRunCommands");
REGISTER_NATIVE_FUNCTION_AS(0x800E4998, MH3_StreamFrame, "StreamFrame");
REGISTER_NATIVE_FUNCTION_AS(0x800EDCB4, MH3_SoundStopAll, "SoundStopAll");
REGISTER_NATIVE_FUNCTION_AS(0x800EDE14, MH3_SoundPauseAll, "SoundPauseAll");
REGISTER_NATIVE_FUNCTION_AS(0x800EDEC4, MH3_SoundResumeAll, "SoundResumeAll");
REGISTER_NATIVE_FUNCTION_AS(0x800E62B4, MH3_SeTick, "SeTick");
REGISTER_NATIVE_FUNCTION_AS(0x800E61CC, MH3_SeUpdate, "SeUpdate");
REGISTER_NATIVE_FUNCTION_AS(0x800E5F28, MH3_SeVoiceDropped, "SeVoiceDropped");
REGISTER_NATIVE_FUNCTION_AS(0x800E5738, MH3_SeVoiceCallback, "SeVoiceCallback");
REGISTER_NATIVE_FUNCTION_AS(0x800E7614, MH3_SeStop, "SeStop");
REGISTER_NATIVE_FUNCTION_AS(0x800E7684, MH3_SePause, "SePause");
REGISTER_NATIVE_FUNCTION_AS(0x800E76EC, MH3_SeResume, "SeResume");
REGISTER_NATIVE_FUNCTION_AS(0x800E6650, MH3_SeStart, "SeStart");
REGISTER_NATIVE_FUNCTION_AS(0x800E7880, MH3_SeSetLevel2, "SeSetLevel2");
REGISTER_NATIVE_FUNCTION_AS(0x800E78DC, MH3_SeSetPitch, "SeSetPitch");
REGISTER_NATIVE_FUNCTION_AS(0x800E6378, MH3_SoundVoiceUpdate3D, "SoundVoiceUpdate3D");
REGISTER_NATIVE_FUNCTION_AS(0x800E5DCC, MH3_SeVolume, "SeVolume");
REGISTER_NATIVE_FUNCTION_AS(0x800E8EC4, MH3_SoundSetPitch, "SoundSetPitch");
REGISTER_NATIVE_FUNCTION_AS(0x800E8F5C, MH3_SoundListenerPolar, "SoundListenerPolar");
REGISTER_NATIVE_FUNCTION_AS(0x800E8FDC, MH3_SoundFalloff, "SoundFalloff");
REGISTER_NATIVE_FUNCTION_AS(0x800E9018, MH3_SoundPan, "SoundPan");
REGISTER_NATIVE_FUNCTION_AS(0x800E9228, MH3_SoundSurroundPan, "SoundSurroundPan");
REGISTER_NATIVE_FUNCTION_AS(0x800E9388, MH3_SoundDistanceVolume, "SoundDistanceVolume");
REGISTER_NATIVE_FUNCTION_AS(0x800E93F4, MH3_SoundDistancePriority, "SoundDistancePriority");
REGISTER_NATIVE_FUNCTION_AS(0x800E9720, MH3_SoundEnvelope, "SoundEnvelope");
REGISTER_NATIVE_FUNCTION_AS(0x800EE078, MH3_SeSetupVoice, "SeSetupVoice");
REGISTER_NATIVE_FUNCTION_AS(0x800E5494, MH3_SeManagerDestroy, "SeManagerDestroy");
REGISTER_NATIVE_FUNCTION_AS(0x800E550C, MH3_SoundManagerDestroy, "SoundManagerDestroy");
REGISTER_NATIVE_FUNCTION_AS(0x800E5568, MH3_SoundManagerBaseDestroy, "SoundManagerBaseDestroy");
REGISTER_NATIVE_FUNCTION_AS(0x800E9DBC, MH3_StreamManagerDestroy, "StreamManagerDestroy");
REGISTER_NATIVE_FUNCTION_AS(0x800ED850, MH3_StreamManagerBaseDestroy, "StreamManagerBaseDestroy");
REGISTER_NATIVE_FUNCTION_AS(0x800ED8AC, MH3_SoundManagerBaseDestroy, "StreamManagerBaseDestroy2");
REGISTER_NATIVE_FUNCTION_AS(0x800E4A44, MH3_SoundClearRequests, "SoundClearRequests");
REGISTER_NATIVE_FUNCTION_AS(0x800E4A58, MH3_SoundObjects, "SoundObjects");
REGISTER_NATIVE_FUNCTION_AS(0x800E4A64, MH3_SoundHasRequest, "SoundHasRequest");
REGISTER_NATIVE_FUNCTION_AS(0x800E4A7C, MH3_StreamManager, "StreamManager");
REGISTER_NATIVE_FUNCTION_AS(0x800E4914, MH3_StepStreamGains, "StepStreamGains");
REGISTER_NATIVE_FUNCTION_AS(0x800E4940, MH3_StreamGainChanged, "StreamGainChanged");
REGISTER_NATIVE_FUNCTION_AS(0x800E4948, MH3_StreamGain, "StreamGain");
REGISTER_NATIVE_FUNCTION_AS(0x800E5778, MH3_SeManager, "SeManager");
REGISTER_NATIVE_FUNCTION_AS(0x800E5F7C, MH3_SoundSetFlags, "SoundSetFlags");
REGISTER_NATIVE_FUNCTION_AS(0x800E5F8C, MH3_SoundClearFlags, "SoundClearFlags");
REGISTER_NATIVE_FUNCTION_AS(0x800E5F9C, MH3_SoundRecord, "SeRecord");
REGISTER_NATIVE_FUNCTION_AS(0x800EA840, MH3_SoundRecord, "StreamRecord");
REGISTER_NATIVE_FUNCTION_AS(0x800E6118, MH3_SeCommandsEnd, "SeCommandsEnd");
REGISTER_NATIVE_FUNCTION_AS(0x800E614C, MH3_SoundSame, "SeCommandsSame");
REGISTER_NATIVE_FUNCTION_AS(0x800E6198, MH3_SoundSame, "SoundObjectsSame");
REGISTER_NATIVE_FUNCTION_AS(0x800E615C, MH3_SeCommandsPending, "SeCommandsPending");
REGISTER_NATIVE_FUNCTION_AS(0x800E9C6C, MH3_SeCommandsPending, "SeCommandsPending2");
REGISTER_NATIVE_FUNCTION_AS(0x800E6164, MH3_SoundObjectsEnd, "SoundObjectsEnd");
REGISTER_NATIVE_FUNCTION_AS(0x800E61A8, MH3_SoundObjectsActive, "SoundObjectsActive");
REGISTER_NATIVE_FUNCTION_AS(0x800EDBAC, MH3_SoundObjectsActive, "SoundObjectsActive2");
REGISTER_NATIVE_FUNCTION_AS(0x800E61B0, MH3_SoundListNext, "SoundObjectNext");
REGISTER_NATIVE_FUNCTION_AS(0x800E9C40, MH3_SoundListNext, "SeCommandNext");
REGISTER_NATIVE_FUNCTION_AS(0x800E61B8, MH3_SeCommandArgs, "SeCommandArgs");
REGISTER_NATIVE_FUNCTION_AS(0x800E61C0, MH3_SeCommands, "SeCommands");
REGISTER_NATIVE_FUNCTION_AS(0x800E629C, MH3_SoundHasFlags, "SoundHasFlags");
REGISTER_NATIVE_FUNCTION_AS(0x800E672C, MH3_SoundMixMode, "SoundMixMode");
REGISTER_NATIVE_FUNCTION_AS(0x800E6734, MH3_SoundMixConfig, "SoundMixConfig");
REGISTER_NATIVE_FUNCTION_AS(0x800E8590, MH3_SeMasterVolume, "SeMasterVolume");
REGISTER_NATIVE_FUNCTION_AS(0x800E85B4, MH3_SeManagerMasterVolume, "SeManagerMasterVolume");
REGISTER_NATIVE_FUNCTION_AS(0x800E85F8, MH3_SeCategoryVolume, "SeCategoryVolume");
REGISTER_NATIVE_FUNCTION_AS(0x800E863C, MH3_SeManagerCategoryVolume, "SeManagerCategoryVolume");
REGISTER_NATIVE_FUNCTION_AS(0x800E86F4, MH3_SoundMasterVolumeAddress, "SoundMasterVolumeAddress");
REGISTER_NATIVE_FUNCTION_AS(0x800E88BC, MH3_SoundMixSoundMode, "SoundMixSoundMode");
REGISTER_NATIVE_FUNCTION_AS(0x800E88C0, MH3_SoundMasterVolume, "SoundMasterVolume");
REGISTER_NATIVE_FUNCTION_AS(0x800E88E4, MH3_SoundReadS16, "SoundReadS16");
REGISTER_NATIVE_FUNCTION_AS(0x800E8EAC, MH3_SoundLevel, "SoundLevel");
REGISTER_NATIVE_FUNCTION_AS(0x800E7D38, MH3_SoundRadians, "SoundRadians");
REGISTER_NATIVE_FUNCTION_AS(0x800E9A74, MH3_SoundListInsert, "SeCommandInsert");
REGISTER_NATIVE_FUNCTION_AS(0x800ED9A0, MH3_SoundListInsert, "SoundObjectInsert");
REGISTER_NATIVE_FUNCTION_AS(0x800E9BC0, MH3_SoundListMoveTo, "SeCommandRetireThunk");
REGISTER_NATIVE_FUNCTION_AS(0x800E9BC4, MH3_SoundListMoveTo, "SeCommandRetire");
REGISTER_NATIVE_FUNCTION_AS(0x800EDB0C, MH3_SoundListMoveTo, "SoundObjectRetire");
REGISTER_NATIVE_FUNCTION_AS(0x800E9C24, MH3_SoundListUnlink, "SeCommandUnlink");
REGISTER_NATIVE_FUNCTION_AS(0x800EDB6C, MH3_SoundListUnlink, "SoundObjectUnlink");
REGISTER_NATIVE_FUNCTION_AS(0x800E9C48, MH3_SeCommandsFirst, "SeCommandsFirst");
REGISTER_NATIVE_FUNCTION_AS(0x800EDB88, MH3_SoundObjectsFirst, "SoundObjectsFirst");
REGISTER_NATIVE_FUNCTION_AS(0x800EDAEC, MH3_SoundFreeObject, "SoundFreeObject");
REGISTER_NATIVE_FUNCTION_AS(0x800EE56C, MH3_SoundCopyAdpcmLoop, "SoundCopyAdpcmLoop");
REGISTER_NATIVE_FUNCTION_AS(0x800EE588, MH3_SoundCopyAdpcm, "SoundCopyAdpcm");
