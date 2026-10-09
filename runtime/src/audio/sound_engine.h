#pragma once

// The game's sound engine (Capcom's sound driver), RMHP08, as native C:
// sound_engine.cpp (the AX frame, the object list, the SE manager, 3D and
// envelope helpers) and sound_stream.cpp (the stream manager). Decompiled
// with Ghidra and checked against the disassembly; build/audio-engine-analysis
// has the call graph and the decompilations.
//
// Every 3 ms AX frame the engine walks its active sound objects: each one's
// manager (SE or stream, two singletons with C++ vtables) frees it once its
// voice has stopped, or ticks it (envelope, 3D pan/volume, stream refill).
// Then it runs the SE command queue (stop/pause/resume/volume/pitch by sound
// id) and the global stop/pause/resume requests of both managers.
//
// The objects, records and queues stay in guest memory with the game's
// layout: the game thread creates and queries them through guest code, and
// the ports are checked against the translated originals (MH3_PORT_CHECK).
// Voices are reached through SoundVoice (sound_voice.h) and SdkMix.
#include "abi_bridge.h"
#include "memory_access.h"

#include <cstdint>

namespace SoundEngine {

using MemoryInline::FlatRead16;
using MemoryInline::FlatRead32;
using MemoryInline::FlatRead8;
using MemoryInline::FlatReadFloat32;
using MemoryInline::FlatWrite16;
using MemoryInline::FlatWrite32;
using MemoryInline::FlatWrite8;
using MemoryInline::FlatWriteFloat32;

// Globals.
constexpr uint32_t kSeManager = 0x80699E00;      // vtable 0x80598568
constexpr uint32_t kStreamManager = 0x8069CC90;  // vtable 0x80598828
constexpr uint32_t kObjects = 0x8069CD68;        // object pool: free list head node, active list node at +0x110
constexpr uint32_t kActive = kObjects + 0x110;
constexpr uint32_t kCommands = 0x8069AF70;       // SE command queue: free list head node, pending list node at +0x4C
constexpr uint32_t kPending = kCommands + 0x4C;
constexpr uint32_t kMixConfig = 0x80699210;      // +0xBEC: the MIX channel mode of every voice
constexpr uint32_t kMasterVolume = 0x80796EC4;   // s16, 0..127
constexpr uint32_t kLevelTable = 0x805985E0;     // s32[128]: level 0..127 -> dB x 10

// Stream buffers: each channel's region is three chunks.
constexpr uint32_t kStreamChannelBytes = 0x80796ED4, kStreamChunkBytes = 0x80796ED8, kStreamEndMargin = 0x80796EDC;

// Stream gain per category (set_str_*_main_vol): selector byte [idx], two
// gains with their previous values.
constexpr uint32_t kStreamGainSelect = 0x80796E70;
constexpr uint32_t kStreamGainA = 0x80796E78, kStreamGainAOld = 0x80796E7C, kStreamGainANew = 0x80796E80;
constexpr uint32_t kStreamGainB = 0x80796E88, kStreamGainBOld = 0x80796E8C, kStreamGainBNew = 0x80796E90;

// Doubly linked list nodes: next +4, prev +8; a list is its head node.
enum : uint32_t { kNext = 4, kPrev = 8 };

// Sound object (the node of the object lists).
enum : uint32_t {
    kObjFlags = 0x0C,
    kObjId = 0x10,
    kObjOwner = 0x14,
    kObjSlot = 0x18,  // u8, 0xFF when free
    kObjType = 0x19,  // u8
    kObjRecord = 0x1C,
};

enum : uint8_t { kTypeSe = 1, kTypeStream = 2 };

// Object flags.
enum : uint32_t {
    kTicking = 1u << 0,       // the manager ticks it
    kStarted = 1u << 8,       // its voice was started
    kPaused = 1u << 9,
    kReleased = 1u << 10,     // stopped: free once the voice is done
    kChunkLoading = 1u << 13, // << chunk: a DVD read fills the chunk
    kChunkLoaded = 1u << 16,  // << chunk: the chunk holds data
    kFadeIn = 1u << 19,
    kFadeOut = 1u << 20,
    kRampTo = 1u << 21,
    kRampBy = 1u << 22,
    kRampScale = 1u << 23,
    kNeedsBuffer = 1u << 24,
    kDvdPaused = 1u << 25,
};

// Manager: global request flags (+4), cleared when served.
enum : uint32_t { kMgrRequests = 4 };
enum : uint32_t { kStopAll = 1, kPauseAll = 2, kResumeAll = 4 };

// Envelope (the SE record, and the stream record + 0x48).
enum : uint32_t {
    kEnvLevel = 0x04,    // s16 0..127
    kEnvAttack = 0x30,   // s16 per tick
    kEnvDecay = 0x32,
    kEnvSustain = 0x34,
    kEnvSlide = 0x36,
    kEnvRelease = 0x38,
    kEnvState = 0x3A,    // u8
};

enum : uint8_t { kEnvStart = 0, kEnvAttacking = 1, kEnvDecaying = 2, kEnvSliding = 3, kEnvHolding = 4, kEnvReleasing = 5 };

inline uint32_t Record(uint32_t object) {
    return object + kObjRecord;
}

inline bool Has(uint32_t object, uint32_t bits) {
    return (FlatRead32(object + kObjFlags) & bits) != 0;
}

inline void Set(uint32_t object, uint32_t bits) {
    FlatWrite32(object + kObjFlags, FlatRead32(object + kObjFlags) | bits);
}

inline void Clear(uint32_t object, uint32_t bits) {
    FlatWrite32(object + kObjFlags, FlatRead32(object + kObjFlags) & ~bits);
}

inline int16_t S16(uint32_t address) {
    return int16_t(FlatRead16(address));
}

inline int32_t Level(int32_t level) {
    return int32_t(FlatRead32(kLevelTable + uint32_t(level) * 4));
}

inline uint32_t MixMode() {
    return FlatRead32(kMixConfig + 0xBEC);
}

// List helpers (0x800ED9A0/0x800E9A74 insert after, 0x800EDB6C/0x800E9C24 unlink).
inline void InsertAfter(uint32_t at, uint32_t node) {
    const uint32_t next = FlatRead32(at + kNext);
    FlatWrite32(node + kNext, next);
    FlatWrite32(next + kPrev, node);
    FlatWrite32(node + kPrev, at);
    FlatWrite32(at + kNext, node);
}

inline void Unlink(uint32_t node) {
    const uint32_t prev = FlatRead32(node + kPrev);
    FlatWrite32(prev + kNext, FlatRead32(node + kNext));
    FlatWrite32(FlatRead32(prev + kNext) + kPrev, prev);
}

// Moves `node` to the front of `list`; returns the node that followed it.
inline uint32_t MoveTo(uint32_t list, uint32_t node) {
    const uint32_t next = FlatRead32(node + kNext);
    Unlink(node);
    InsertAfter(list, node);
    return next;
}

// A registered entry point: the port, checked against the translated
// original when MH3_PORT_CHECK names it. Development: with its
// `originals` option the entry points not named there run their
// originals, so a named port is reached from original callers (the ports
// call each other directly, which the checks would not see).
void Entry(CpuContext* cpu, const char* name, void (*original)(CpuContext*), void (*port)(CpuContext*));

// Entry for a port that only returns r3 (PortCheck::RunPure).
void EntryPure(CpuContext* cpu, const char* name, void (*original)(CpuContext*), void (*port)(CpuContext*));

// 0x800EDAEC: returns a sound object to the pool; returns the next active one.
uint32_t FreeObject(uint32_t object);

// 0x800E9720: one envelope step; may start or stop the object.
void Envelope(CpuContext* cpu, uint32_t manager, uint32_t object, uint32_t envelope);

// The managers' methods (vtable slots), by manager.
void ManagerStop(CpuContext* cpu, uint32_t manager, uint32_t object);    // +0x18
void ManagerPause(CpuContext* cpu, uint32_t manager, uint32_t object);   // +0x1C
void ManagerResume(CpuContext* cpu, uint32_t manager, uint32_t object);  // +0x20

// The managers' voice drop handlers (vtable +0x14): AX stole `voice` from
// the sound `object`.
void SeVoiceDropped(uint32_t voice, uint32_t object);

// Stream manager methods (sound_stream.cpp).
namespace Stream {
void VoiceDropped(CpuContext* cpu, uint32_t voice, uint32_t object);
void Tick(CpuContext* cpu, uint32_t manager, uint32_t object);
bool Update(CpuContext* cpu, uint32_t manager, uint32_t& object);
void Stop(CpuContext* cpu, uint32_t manager, uint32_t object);
void Pause(uint32_t object);
void Resume(uint32_t object);
void Start(uint32_t manager, uint32_t object);
} // namespace Stream

} // namespace SoundEngine
